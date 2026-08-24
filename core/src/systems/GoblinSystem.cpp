#include "core/systems/GoblinSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "core/Body.hpp"
#include "core/Carcass.hpp"
#include "core/Desires.hpp"
#include "core/Diagnostics.hpp"
#include "core/Hunting.hpp"
#include "core/Mating.hpp"
#include "core/Needs.hpp"
#include "core/Path.hpp"
#include "core/Random.hpp"
#include "core/Scale.hpp"
#include "core/Share.hpp"
#include "core/TileSnapshot.hpp"
#include "core/Walk.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/GoblinDesireComponent.hpp"
#include "core/components/GoblinTribesComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/GoblinGenetics.hpp"

namespace goblins {

namespace {

// Пороги желаний. Числа те же, что у животных, но константы свои, и это не
// оплошность: у гоблина скоро появятся желания, которых у зверя нет и быть
// не может (отдых, ноша, работа), и равновесие между ними придётся крутить
// отдельно. Общим здесь остаётся закон (core/Desires.hpp), а не значения.
constexpr int kDesireFloor = 350;
constexpr int kDesireSwitch = 150;

// Продолжение рода. Желание копится только у взрослого, доросшего и не
// бедствующего (kCalmNeed — предел голода и жажды, при котором ещё не
// бедствие), а идти искать пару гоблин начинает с kMateDesire.
constexpr int kBreedingGrowth = 900;
constexpr int kCalmNeed = 750;
constexpr int kMateDesire = 600;

// С какой вероятностью ничего не желающий гоблин всё-таки делает шаг.
// Постоянно бродящий выглядит нервным и зря жжёт энергию, полностью
// неподвижный — мёртвым.
constexpr int kWanderChance = 250;

// Сколько тиков гоблин держит одно направление поиска, когда желаемого не
// видно. Единственный способ найти что-то за пределами своей видимости:
// случайный шаг в случайную сторону уводит от исходной точки как корень из
// числа шагов, а прямая ходьба — линейно.
//
// Со шага "память места" этот способ станет запасным: у гоблина появится
// куда возвращаться, и блуждание останется тем, чем оно и является, —
// способом узнать новое, а не способом дойти до известного.
constexpr std::uint64_t kRoamTicks = 40;

// --- Намерения ---
// Собираются при обходе гоблинов и исполняются после него: на один куст,
// одну тушу и один водопой могут прийти сразу несколько, и решать спор
// порядком обхода Entity нельзя (04_WorldModel.md, п.8). Дележ — общий
// закон (core/Share.hpp, ShareIntent).

struct StepIntent {
    int goblin = 0;
    int x = 0;
    int y = 0;
};

struct MateIntent {
    std::size_t cell = 0;
    int goblin = 0;
    std::uint64_t id = 0;
    int tribe = 0;
    Sex sex = Sex::Female;
};

// Живой гоблин в снимке этого тика. Указатели на компоненты держать
// безопасно: за время обхода систем структура хранилища не меняется —
// создание и удаление Entity идёт только через очередь команд
// (05_Entity.md, п.5).
struct Goblin {
    entt::entity entity = entt::null;
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    AnimalComponent* state = nullptr;
    const AnimalGenomeComponent* genome = nullptr;
    GoblinDesireComponent* desire = nullptr;
    MovementComponent* memory = nullptr;

    // Голод и жажда живут здесь, в снимке тика, а не в компоненте: оба
    // пересчитываются из тела заново каждый тик (core/Needs.hpp), и
    // пережить тик им незачем.
    int hunger = 0;
    int thirst = 0;
};

// Какое желание сейчас гонит гоблина. Сам выбор — общий закон мира
// (core/Desires.hpp); здесь только то, чего гоблин может хотеть и чем
// меряется срочность каждого.
//
// Страха в этом списке нет, и это не забывчивость: бояться гоблину пока
// некого — хищник его не видит (см. GoblinSystem.hpp). Появится опасность —
// появится и желание, и встанет оно последним, чтобы побеждать при
// равенстве.
GoblinDesire chooseGoblinDesire(const Goblin& goblin, bool readyToMate) {
    const GoblinDesireComponent& desire = *goblin.desire;
    const int mating = readyToMate && desire.mating >= kMateDesire ? desire.mating : 0;

    const Urgency candidates[] = {
        {static_cast<int>(GoblinDesire::Food), goblin.hunger},
        {static_cast<int>(GoblinDesire::Water), goblin.thirst},
        {static_cast<int>(GoblinDesire::Mate), mating},
    };

    int currentUrgency = 0;
    switch (desire.current) {
        case GoblinDesire::Food: currentUrgency = goblin.hunger; break;
        case GoblinDesire::Water: currentUrgency = goblin.thirst; break;
        case GoblinDesire::Mate: currentUrgency = mating; break;
        case GoblinDesire::Idle: break;
    }

    return static_cast<GoblinDesire>(chooseUrgent(candidates, static_cast<int>(desire.current), currentUrgency,
                                                   kDesireFloor, kDesireSwitch,
                                                   static_cast<int>(GoblinDesire::Idle)));
}

} // namespace

void GoblinSystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    auto& registry = world.registry();
    const auto& worldProperties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    // Мутация в тысячных долях вложения (core/Scale.hpp) — сам расклад
    // бюджета дробный, это генерация, а не состояние мира.
    const float mutationRate = static_cast<float>(worldProperties.goblinMutationRate) / kFull;
    const auto goblinSeed = static_cast<std::uint64_t>(worldProperties.goblinRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& tribes = registry.get<const GoblinTribesComponent>(world.worldEntity());

    // --- 1. Снимок гоблинов ---
    // Разреженно, а не плотным массивом на всю Область: гоблинов десятки, а
    // клеток десятки тысяч.
    std::vector<Goblin> goblins;
    auto goblinView = registry.view<AnimalComponent, AnimalGenomeComponent, GoblinDesireComponent,
                                     IdentityComponent, MovementComponent, PositionComponent, GoblinComponent>();
    for (const auto entity : goblinView) {
        const auto& position = goblinView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        goblins.push_back(Goblin{entity, goblinView.get<IdentityComponent>(entity).id, position.x, position.y,
                                  &goblinView.get<AnimalComponent>(entity),
                                  &goblinView.get<AnimalGenomeComponent>(entity),
                                  &goblinView.get<GoblinDesireComponent>(entity),
                                  &goblinView.get<MovementComponent>(entity)});
    }
    // Гоблинов нет — делать системе нечего. В отличие от AnimalSystem, за
    // которой числится ещё и гниение падали, у этой своих обязанностей перед
    // миром нет ни одной: она только про гоблинов.
    if (goblins.empty()) {
        return;
    }

    // --- 2. Снимок тайлов ---
    // Свой, а не общий с AnimalSystem, и снимается сейчас, а не в начале
    // тика: стадо уже поело, и гоблин обязан видеть то, что от куста
    // осталось (05_Entity.md, п.6 — системы разговаривают состоянием
    // компонентов).
    TileSnapshot tiles;
    tiles.capture(world);
    const std::vector<entt::entity>& terrain = tiles.terrain;
    const std::vector<int>& waterAt = tiles.waterAt;
    const std::vector<entt::entity>& plantAt = tiles.plantAt;
    const std::vector<int>& plantGrowth = tiles.plantGrowth;
    const std::vector<int>& carcassMeat = tiles.carcassMeat;

    std::vector<ShareIntent> bites;  // трава
    std::vector<ShareIntent> meals;  // падаль
    std::vector<ShareIntent> drinks;
    std::vector<StepIntent> steps;
    std::vector<MateIntent> matings;

    // --- 3. Тело и желания ---
    // Отдельным проходом от решений (п.4) намеренно: гоблин, выбирая пару,
    // смотрит, чего хочет сосед, — и если бы желания и решения считались в
    // одном проходе, сосед, которого EnTT хранит позже, был бы ещё с
    // прошлотиковым желанием. Порядок в памяти не может быть причиной
    // события в мире (02_CorePrinciples.md, п.12a).
    std::vector<bool> alive(goblins.size(), true);
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        Goblin& goblin = goblins[g];
        auto& state = *goblin.state;
        const auto& genome = *goblin.genome;
        auto& desire = *goblin.desire;

        advanceBody(state, genome, tick, goblin.id);

        // Своих бед сверх общего закона у гоблина пока нет: болезни от
        // тесноты он не знает (поселение тесно по сути), зубов на него никто
        // не точит. Поэтому между телом и смертью здесь ничего и не стоит.
        if (bodyDied(state, genome)) {
            enqueueDeath(commands, goblin.entity, goblin.x, goblin.y);
            alive[g] = false;
            continue;
        }

        // Память ног тает со временем, а не от шагов (core/Walk.hpp):
        // простоявший сотню тиков у куста не должен помнить преграду,
        // которой давно нет.
        fadeWalkMemory(*goblin.memory);

        goblin.hunger = hungerOf(state, genome);
        goblin.thirst = thirstOf(state, genome);

        const bool adult = state.age >= genome.maturityAge && state.growth >= kBreedingGrowth;
        const bool content = state.health >= kFull && goblin.hunger < kCalmNeed && goblin.thirst < kCalmNeed;
        if (adult && content) {
            desire.mating = std::min(kFull, desire.mating + genome.breedingUrge);
        }
        desire.current = chooseGoblinDesire(goblin, adult && content);
    }

    // Возможная пара в том виде, в каком её видно со стороны
    // (core/Mating.hpp). Желания у всех уже посчитаны (п.3), поэтому
    // "согласен" здесь честное, а не прошлотиковое.
    //
    // Хищником не назван никто: закон встречи различает диеты, потому что у
    // животных ими различаются виды, а гоблины все одной таблицы —
    // различает их племя, и оно едет в поле species.
    std::vector<MateCandidate> mates;
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        mates.push_back(MateCandidate{goblins[g].id, goblins[g].x, goblins[g].y, goblins[g].genome->species,
                                       false, goblins[g].state->sex,
                                       alive[g] && goblins[g].desire->current == GoblinDesire::Mate});
    }

    // Округа и дорога по ней (core/Path.hpp). Живут снаружи цикла и
    // переиспользуются: за тик волна пускается столько раз, сколько в мире
    // ищущих, а массивы у неё на всю Область.
    Reach reachOf;
    std::vector<PathCell> road;

    // --- 4. Решения: что гоблин делает со своим желанием ---
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        if (!alive[g]) {
            continue;
        }
        const Goblin& goblin = goblins[g];
        auto& state = *goblin.state;
        const auto& genome = *goblin.genome;
        auto& desire = *goblin.desire;
        const std::size_t here = index(goblin.x, goblin.y);
        const int size = bodySize(state.growth);

        // Случайность собирается из seed мира, номера тика и постоянного
        // идентификатора (core/Random.hpp). Не из координат: клетка меняется
        // каждый шаг, а на одной клетке их может стоять несколько — розыгрыш
        // вышел бы одинаковым.
        std::uint64_t random = mixSeed(goblinSeed, mixSeed(tick, goblin.id));

        const int reach = std::max(1, genome.perception);
        bool busy = false;
        bool hasTarget = false;
        int targetX = goblin.x;
        int targetY = goblin.y;

        // Куда гоблин вообще может встать (core/Path.hpp, standableAt): не
        // за границей Области, не на занятый непроходимым объектом тайл и не
        // в воду. Правило общее, здесь только факты, из которых оно
        // складывается: снимок тайлов этого тика.
        auto standable = [&](int nx, int ny) {
            if (!world.area().inBounds(nx, ny)) {
                return false;
            }
            const std::size_t cell = index(nx, ny);
            return standableAt(world.area().isBlocked(nx, ny), terrain[cell] != entt::null, waterAt[cell]);
        };

        // Ближайшая клетка в пределах видимости, удовлетворяющая условию.
        // Ближайшая, а не лучшая: гоблин идёт к тому, что видит рядом, а не
        // выбирает оптимум по всей округе. Из одинаково близких выбор
        // бросается жребием — иначе обход, идущий с левого верхнего угла,
        // уводил бы всех дружно вверх и влево.
        auto findNearest = [&](auto predicate, int& outX, int& outY) {
            int bestDistance = 0;
            int ties = 0;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int nx = goblin.x + dx;
                    const int ny = goblin.y + dy;
                    if (!world.area().inBounds(nx, ny)) {
                        continue;
                    }
                    const int distance = dx * dx + dy * dy;
                    if (distance > reach * reach) {
                        continue; // видимость круглая, а не квадратная
                    }
                    if (ties > 0 && distance > bestDistance) {
                        continue;
                    }
                    if (!predicate(index(nx, ny), nx, ny)) {
                        continue;
                    }
                    if (ties > 0 && distance == bestDistance) {
                        ++ties;
                        if (randomBelow(random, static_cast<std::uint64_t>(ties)) != 0) {
                            continue;
                        }
                    } else {
                        ties = 1;
                    }
                    bestDistance = distance;
                    outX = nx;
                    outY = ny;
                }
            }
            return ties > 0 ? bestDistance : -1;
        };

        switch (desire.current) {
            case GoblinDesire::Food: {
                // Мясо под ногами — раньше травы под ногами: туша это
                // десяток кустов разом (kMeatPerSize, core/Carcass.hpp), и
                // пренебречь ею ради пучка травы значило бы оставить её
                // гнить. Живое гоблин при этом не бьёт — он подбирает
                // мёртвое.
                if (carcassMeat[here] > kMinBiteMeat) {
                    meals.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, genome.biteSize * size / kFull});
                    busy = true;
                    break;
                }
                if (plantAt[here] != entt::null && plantGrowth[here] > kMinBiteGrowth) {
                    bites.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, genome.biteSize * size / kFull});
                    busy = true;
                    break;
                }

                // Под ногами пусто — ищем глазами. Сначала падаль: она
                // редка, лежит в одной точке, и идти к ней надо дорогой
                // (core/Path.hpp), иначе увиденная через реку заведёт
                // гоблина на берег и оставит там. Волна считается только
                // тогда, когда есть на что смотреть: перебор клеток дёшев, а
                // волна по округе — нет.
                int meatX = goblin.x;
                int meatY = goblin.y;
                const bool meatSeen =
                    findNearest([&](std::size_t cell, int nx, int ny) {
                        return carcassMeat[cell] > kMinBiteMeat && standable(nx, ny);
                    }, meatX, meatY) >= 0;
                if (meatSeen) {
                    reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                    if (reachOf.reached(meatX, meatY)) {
                        reachOf.roadTo(meatX, meatY, road);
                        if (!road.empty()) {
                            targetX = road.front().x;
                            targetY = road.front().y;
                            hasTarget = true;
                        }
                    }
                }

                // Травы в мире много, и упираться в берег ради одного
                // конкретного куста незачем: к ней идут напрямик, а преграду
                // обходят вслепую памятью ног. Тот же выбор, что у
                // травоядного, и по той же причине.
                if (!hasTarget) {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return plantAt[cell] != entt::null && plantGrowth[cell] > kMinBiteGrowth &&
                                               standable(nx, ny);
                                    },
                                    targetX, targetY) >= 0;
                }
                break;
            }
            case GoblinDesire::Water: {
                // Пьёт со своей клетки или с любой соседней: гоблин стоит на
                // берегу, а не заходит в реку — шагнуть в воду он и не может
                // (см. standable). Своя клетка в проверке всё равно нужна:
                // паводок может залить ту, на которой он стоит.
                std::size_t source = cellCount;
                if (waterAt[here] > 0) {
                    source = here;
                } else {
                    for (int dir = 0; dir < 8; ++dir) {
                        const int nx = goblin.x + kWalkX[dir];
                        const int ny = goblin.y + kWalkY[dir];
                        if (!world.area().inBounds(nx, ny)) {
                            continue;
                        }
                        const std::size_t j = index(nx, ny);
                        if (waterAt[j] > 0) {
                            source = j;
                            break;
                        }
                    }
                }
                if (source < cellCount) {
                    drinks.push_back(
                        ShareIntent{source, static_cast<int>(g), goblin.id, kDrinkRate * size / kFull});
                    busy = true;
                } else {
                    hasTarget = findNearest([&](std::size_t cell, int, int) { return waterAt[cell] > 0; },
                                             targetX, targetY) >= 0;
                }
                break;
            }
            case GoblinDesire::Mate: {
                // Пару ищут дорогой (core/Mating.hpp): увиденное через реку —
                // ещё не найденное. Пара за водой видна обоим, сойтись им
                // негде, и оба стоят до конца жизни в двадцати шагах друг от
                // друга.
                const Suitor suitor{goblin.id, goblin.x, goblin.y, reach, genome.species, false, state.sex};
                if (!anyMateInSight(suitor, mates)) {
                    // Рядом никого не видно — но зовущая слышна дальше, чем
                    // видна. Цель ставится прямо, без дороги: звук не
                    // спрашивает брода, а как дойти, решит сам шаг.
                    const MateChoice call = hearCall(suitor, mates);
                    if (call.found) {
                        targetX = call.x;
                        targetY = call.y;
                        hasTarget = true;
                    }
                    break;
                }
                reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                const MateChoice mate = chooseMate(reachOf, suitor, mates);
                if (!mate.found) {
                    break;
                }
                // Сошлись — встреча случилась на этой клетке. Кто с кем
                // именно, решится ниже (п.9), когда соберутся все: намерение
                // здесь не называет второго, потому что на одной клетке их
                // может ждать и трое.
                if (mate.x == goblin.x && mate.y == goblin.y) {
                    matings.push_back(
                        MateIntent{here, static_cast<int>(g), goblin.id, genome.species, state.sex});
                    busy = true;
                    break;
                }
                reachOf.roadTo(mate.x, mate.y, road);
                if (!road.empty()) {
                    targetX = road.front().x;
                    targetY = road.front().y;
                    hasTarget = true;
                }
                break;
            }
            case GoblinDesire::Idle: break;
        }

        // --- Шаг ---
        // Занятый (ест, пьёт, сошёлся с парой) с места не сходит.
        if (busy) {
            continue;
        }

        // Скорость — тысячных клетки за тик (core/Scale.hpp): копится, пока
        // не наберётся целая клетка. Одна клетка за тик и не больше; отсюда
        // и потолок — невыбранный запас иначе рос бы без конца, а шагов от
        // этого не прибавлялось бы.
        state.stepProgress = std::min(state.stepProgress + genome.speed, 2 * kFull - 1);
        if (state.stepProgress < kFull) {
            continue;
        }
        state.stepProgress -= kFull;

        // Направление поиска, когда желаемого не видно. Гоблин идёт в одну
        // сторону целый отрезок пути (kRoamTicks), а не топчется на месте.
        // Берётся из постоянного идентификатора и номера отрезка, поэтому
        // системе не нужно ничего помнить между тиками (05_Entity.md, п.3).
        auto roamDirection = [&]() {
            std::uint64_t roam = mixSeed(goblin.id, tick / kRoamTicks);
            return static_cast<int>(nextState(roam) % 8u);
        };

        // Куда гоблин хочет — ОДНО направление на все случаи движения, и
        // дальше шаг считается одинаково (core/Walk.hpp). Идущий к цели,
        // ищущий за пределами видимости и просто бродящий отличаются только
        // тем, откуда взялось это направление; обход преграды получается
        // сам.
        int aim = -1;
        if (hasTarget) {
            aim = walkDirectionTo(goblin.x, goblin.y, targetX, targetY);
        } else if (desire.current != GoblinDesire::Idle) {
            aim = roamDirection();
        } else if (static_cast<int>(randomBelow(random, kFull)) >= kWanderChance) {
            continue; // ничего не гонит — стоит
        }

        // Сторониться гоблину пока некого: чужое племя ему не соперник за
        // траву настолько, чтобы обходить его стороной, а хищника он не
        // видит. Поэтому WalkShy пустой — но он есть, и в него встанет
        // первая же причина держаться подальше.
        const WalkStep step = chooseStep(*goblin.memory, goblin.x, goblin.y, aim, WalkShy{}, standable, random);
        if (!step.moved) {
            continue; // шагнуть некуда вовсе: вода, камень или край мира
        }

        state.energy = std::max(0, state.energy - kStepEnergy * size / kFull);
        steps.push_back(StepIntent{static_cast<int>(g), step.x, step.y});
    }

    // --- 5. Кормёжка травой: один куст на всех, кто до него дотянулся ---
    std::sort(bites.begin(), bites.end(), sortByCellThenId);
    for (std::size_t n = 0; n < bites.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < bites.size() && bites[m].cell == bites[n].cell) {
            demand += bites[m].want;
            ++m;
        }

        const entt::entity plantEntity = plantAt[bites[n].cell];
        auto* plant = registry.valid(plantEntity) ? registry.try_get<PlantComponent>(plantEntity) : nullptr;
        if (plant == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        const int growthBefore = plant->growth;
        const int mineralsBefore = plant->minerals;

        int eatenTotal = 0;
        int releasedTotal = 0;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(bites[k].want, growthBefore, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(bites[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(bites[k].claimant)].genome;

            feedBody(state, genome, eaten);
            eatenTotal += eaten;

            const int owed =
                growthBefore > 0 ? std::min(mineralsBefore, mineralsBefore * eatenTotal / growthBefore) : 0;
            int taken = 0;
            for (int grain = releasedTotal; grain < owed && plant->minerals > 0; ++grain) {
                --plant->minerals;
                ++taken;
            }
            takeProtein(state, genome, taken);
            releasedTotal = std::max(releasedTotal, owed);
        }

        // Объедание отнимает биомассу, и только её: погибнуть от зубов куст
        // не может, отрастёт он или нет — решит PlantSystem на следующем
        // тике.
        plant->growth = std::max(0, plant->growth - eatenTotal);
        n = m;
    }

    // --- 6. Кормёжка падалью: одна туша на всех, кто до неё добрался ---
    std::sort(meals.begin(), meals.end(), sortByCellThenId);
    for (std::size_t n = 0; n < meals.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < meals.size() && meals[m].cell == meals[n].cell) {
            demand += meals[m].want;
            ++m;
        }

        const entt::entity tile = terrain[meals[n].cell];
        auto* carcass =
            tile != entt::null && registry.valid(tile) ? registry.try_get<CarcassComponent>(tile) : nullptr;
        if (carcass == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        const int meatBefore = carcass->meat;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(meals[k].want, meatBefore, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(meals[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(meals[k].claimant)].genome;

            feedBody(state, genome, eaten);

            const int meatNow = carcass->meat;
            carcass->meat = std::max(0, carcass->meat - eaten);
            takeProtein(state, genome, releaseCarcassProtein(*carcass, meatNow, meatNow - carcass->meat));
        }
        n = m;
    }

    // --- 7. Водопой: так же долями ---
    // Клетку при этом не вычерпывают: река живёт по своему закону
    // (HydrologySystem), и водопой в этот баланс не входит.
    std::sort(drinks.begin(), drinks.end(), sortByCellThenId);
    for (std::size_t n = 0; n < drinks.size();) {
        std::size_t m = n;
        while (m < drinks.size() && drinks[m].cell == drinks[n].cell) {
            ++m;
        }
        const entt::entity tile = terrain[drinks[n].cell];
        auto* water = tile != entt::null && registry.valid(tile) ? registry.try_get<WaterComponent>(tile) : nullptr;
        if (water == nullptr) {
            n = m;
            continue;
        }
        for (std::size_t k = n; k < m; ++k) {
            auto& state = *goblins[static_cast<std::size_t>(drinks[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(drinks[k].claimant)].genome;
            state.water = std::min(genome.waterCapacity, state.water + drinks[k].want);
        }
        n = m;
    }

    // --- 8. Шаги ---
    // Собранные намерения исполняются разом, после всех решений: иначе
    // сдвинувшийся гоблин менял бы обстановку тем, кто решает после него.
    for (const auto& step : steps) {
        const auto s = static_cast<std::size_t>(step.goblin);
        if (!alive[s]) {
            continue;
        }
        world.moveTo(goblins[s].entity, step.x, step.y);
    }

    // --- 9. Встречи: кто с кем сошёлся ---
    // Пары складываются внутри клетки, в порядке постоянных
    // идентификаторов: если на одной клетке сошлись трое, кому достанется
    // пара — вопрос их собственных имён в мире, а не порядка в хранилище.
    std::sort(matings.begin(), matings.end(), [](const MateIntent& a, const MateIntent& b) {
        if (a.cell != b.cell) {
            return a.cell < b.cell;
        }
        return a.id < b.id;
    });
    std::vector<bool> paired(matings.size(), false);
    for (std::size_t n = 0; n < matings.size(); ++n) {
        if (paired[n] || !alive[static_cast<std::size_t>(matings[n].goblin)]) {
            continue;
        }
        std::size_t partner = matings.size();
        for (std::size_t k = n + 1; k < matings.size() && matings[k].cell == matings[n].cell; ++k) {
            if (paired[k] || !alive[static_cast<std::size_t>(matings[k].goblin)] ||
                matings[k].tribe != matings[n].tribe || matings[k].sex == matings[n].sex) {
                continue;
            }
            partner = k;
            break;
        }
        if (partner == matings.size()) {
            continue;
        }
        paired[n] = true;
        paired[partner] = true;

        const bool firstIsMother = matings[n].sex == Sex::Female;
        const Goblin& mother =
            goblins[static_cast<std::size_t>(firstIsMother ? matings[n].goblin : matings[partner].goblin)];
        const Goblin& father =
            goblins[static_cast<std::size_t>(firstIsMother ? matings[partner].goblin : matings[n].goblin)];

        const auto& motherGenome = *mother.genome;
        const auto& fatherGenome = *father.genome;
        std::uint64_t random = mixSeed(goblinSeed, mixSeed(tick, mixSeed(mother.id, father.id)));

        const auto& archetypes = tribes.tribes;
        const auto& archetype =
            (motherGenome.species >= 0 && static_cast<std::size_t>(motherGenome.species) < archetypes.size())
                ? archetypes[static_cast<std::size_t>(motherGenome.species)]
                : motherGenome;
        // Скрещивание — общий закон (core/generation/AnimalGenetics.hpp), а
        // таблица черт своя: она и есть вся разница между гоблином и зверем
        // в наследовании.
        const AnimalGenomeComponent childGenome =
            crossGenomes(goblinTraits(), motherGenome, fatherGenome, archetype, mutationRate, random);

        AnimalComponent child;
        child.growth = kNewbornGrowth;
        child.sex = randomBelow(random, 2) == 0 ? Sex::Female : Sex::Male;

        // Ребёнок появляется не из ниоткуда: и запасы, и белок — материнские.
        // Ровно тот же обмен, что у растения с семенем и у зверя с
        // детёнышем. Отец не платит ничего.
        const int givenEnergy = mother.state->energy * kBirthEnergyShare / kFull;
        mother.state->energy -= givenEnergy;
        child.energy = std::min(givenEnergy, childGenome.energyCapacity);

        const int givenWater = mother.state->water * kBirthWaterShare / kFull;
        mother.state->water -= givenWater;
        child.water = std::min(givenWater, childGenome.waterCapacity);

        const int givenProtein = std::max(0, mother.state->protein / 3);
        mother.state->protein -= givenProtein;
        child.protein = givenProtein;
        child.growth = childGenome.proteinNeed > 0
                            ? std::min(child.growth, child.protein * kFull / childGenome.proteinNeed)
                            : child.growth;

        mother.desire->mating = 0;
        father.desire->mating = 0;
        mother.desire->current = GoblinDesire::Idle;
        father.desire->current = GoblinDesire::Idle;

        const std::uint64_t childId = mixSeed(random, mixSeed(mother.id, tick));
        commands.enqueue([child, childGenome, childId, x = mother.x, y = mother.y](World& w) {
            const auto entity = w.registry().create();
            w.registry().emplace<IdentityComponent>(entity, IdentityComponent{childId});
            w.registry().emplace<AnimalComponent>(entity, child);
            w.registry().emplace<AnimalGenomeComponent>(entity, childGenome);
            // Новорождённый ничего ещё не хочет — тело у него полное; чего
            // хотеть, ему скажет первый же тик. И ногами он пока ничего не
            // помнит: ни шага, ни преграды.
            w.registry().emplace<GoblinDesireComponent>(entity, GoblinDesireComponent{});
            w.registry().emplace<MovementComponent>(entity);
            w.registry().emplace<GoblinComponent>(entity);
            // Проверять клетку не нужно: существо не занимает тайл
            // (04_WorldModel.md, п.4), поэтому ребёнок всегда помещается
            // рядом с матерью.
            w.place(entity, x, y);
        });
    }
}

// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
// Того, что общее со зверем, здесь нет: оно перечислено в группе животных,
// потому что живёт в core/Body.hpp и одинаково для обоих.
void appendGoblinSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Goblins (tick)";
    out.push_back({g, "kDesireFloor", kDesireFloor});
    out.push_back({g, "kDesireSwitch", kDesireSwitch});
    out.push_back({g, "kBreedingGrowth", kBreedingGrowth});
    out.push_back({g, "kCalmNeed", kCalmNeed});
    out.push_back({g, "kMateDesire", kMateDesire});
    out.push_back({g, "kWanderChance", kWanderChance});
    out.push_back({g, "kRoamTicks", static_cast<float>(kRoamTicks)});
}

} // namespace goblins

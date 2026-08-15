#include "core/systems/HerbivoreSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Humus.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/HerbivoreGenomeComponent.hpp"
#include "core/components/HerbivoreSpeciesComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/HerbivoreGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Насколько маленькое животное "меньше ест": и расход на жизнь, и укус, и
// цена шага, и скорость пищеварения умножаются на размер (от kMinSizeShare
// у новорождённого до 1 у взрослого) — тот же приём, что у растений. Без
// него телёнок объедал бы луг наравне со взрослым и почти никогда не
// выживал бы на бедной земле.
constexpr float kMinSizeShare = 0.3f;

// Сколько энергии даёт единица развитости съеденного растения (до того, как
// её умножат на digestion генома). Число связывает две шкалы, которые
// иначе не сопоставимы: развитость растения живёт в [0, 1], а расход
// животного — в единицах за тик. При нынешних значениях взрослый куст
// кормит животное примерно на пару сотен тиков — то есть травоядное
// съедает считанные растения за поколение травы, а не выкашивает луг.
constexpr float kEnergyPerBiomass = 40.0f;

// Сколько воды животное получает из самой травы: трава сочная, и часть
// жажды утоляется кормёжкой. Множитель к влаге, накопленной растением
// (PlantComponent::moisture), в пересчёте на съеденную долю куста. Полностью
// заменить водопой этим нельзя — запас растения слишком мал.
constexpr float kWaterPerBiomass = 6.0f;

// Цена одного шага в энергии (взрослому). Вместе со скоростью из генома это
// и есть плата за быстроту: быстрое животное делает больше шагов за тик,
// значит и тратит больше — скорость покупается не только бюджетом
// преимуществ, но и обменом веществ.
constexpr float kStepEnergy = 0.4f;

// Водопой: сколько единиц воды животное выпивает за тик и сколько глубины
// это снимает с клетки. Второе число мало намеренно: стадо не должно
// выпивать реку — оно и не выпивает, но след в мире оставляет, и вода
// приходит не из ниоткуда.
constexpr float kDrinkRate = 4.0f;
constexpr float kDrinkDepthPerUnit = 0.002f;

// Ниже этой развитости куст объедать нечего — животное его просто не
// замечает как еду. Иначе стадо паслось бы на голых проростках, не давая
// лугу отрасти вовсе.
constexpr float kMinBiteGrowth = 0.05f;

// Сколько стресса получает растение за единицу съеденной развитости. Именно
// так объедание убивает траву — не напрямую, а через её собственный закон
// смерти от условий (PlantSystem): куст, который скусывают снова и снова,
// не успевает отрасти и в конце концов погибает, а редко потревоженный
// отходит.
constexpr float kGrazeStress = 0.4f;

// Пищеварение: какая доля крупицы за тик проходит через взрослое животное
// в навоз. Отсюда и необходимость есть постоянно: белок не лежит в теле
// вечно. Навоз выпадает не по крупице, а порциями в kDungDrop крупиц —
// это навоз, а не равномерная плёнка по всему маршруту.
constexpr float kDungRate = 0.004f;
constexpr int kDungDrop = 2;

// Стресс. Голод убивает примерно за 1/kStarvationStress тиков, жажда
// быстрее — без воды живут меньше, чем без еды. Восстановление медленнее
// накопления: пережитая бескормица не забывается мгновенно.
constexpr float kStarvationStress = 0.02f;
constexpr float kDehydrationStress = 0.03f;
constexpr float kStressRelief = 0.01f;

// Желания. Ниже kDesireFloor желание никуда не гонит — животное считается
// довольным и просто бродит. kDesireSwitch — насколько сильнее должно быть
// другое желание, чтобы перебить уже выбранное: без этого запаса животное с
// почти равными голодом и жаждой каждый тик разворачивалось бы и не дошло
// бы ни до травы, ни до воды.
constexpr float kDesireFloor = 0.35f;
constexpr float kDesireSwitch = 0.15f;

// Размножение. Желание пары копится только у взрослого, доросшего и не
// бедствующего животного (kCalmNeed — предел голода и жажды, при котором
// ещё до того), а идти искать партнёра оно начинает с kMateDesire.
constexpr float kBreedingGrowth = 0.9f;
constexpr float kCalmNeed = 0.4f;
constexpr float kMateDesire = 0.6f;

// Цена потомства. Мать отдаёт телёнку долю своих запасов и крупицу белка —
// ровно как растение отдаёт семени часть влаги и крупицу минералов; отец
// платит только ухаживанием (энергия тратится, но никуда не переходит).
// Именно эта цена, а не отдельный "лимит поголовья", и сдерживает
// размножение: после отёла матери нужно заново отъедаться.
constexpr float kBirthEnergyShare = 0.4f;
constexpr float kBirthWaterShare = 0.3f;
constexpr float kCourtshipEnergyShare = 0.15f;
constexpr float kNewbornGrowth = 0.08f;

// Насколько один проход животного уплотняет почву под ногами. Черта
// "терпимость к утоптанности" (PlantGenomeComponent::compactionTolerance)
// была куплена растениями за бюджет ещё до появления животных и до сих пор
// ничего не значила: топтать почву было некому (docs/08_Plants.md, п.9).
// Теперь есть кому — тропа к водопою становится твёрдой, и трава на ней
// растёт хуже.
constexpr float kTrampleRate = 0.004f;

// С какой вероятностью ничего не желающее животное всё-таки делает шаг.
// Постоянно бродящее стадо выглядит нервным и зря жжёт энергию; полностью
// неподвижное — мёртвым.
constexpr float kWanderChance = 0.25f;

// --- Намерения ---
// Собираются при обходе животных и исполняются после него. Отдельный шаг
// нужен там же, где и в PlantSystem: на один куст и на один водопой могут
// претендовать сразу несколько животных, и решать спор порядком обхода
// Entity нельзя (04_WorldModel.md, п.8). Спор делится долями: если
// желаемого меньше, чем просят, каждый получает свою часть — исход не
// зависит ни от порядка обхода, ни от того, кого EnTT хранит раньше.
struct ShareIntent {
    std::size_t cell = 0;      // клетка с кустом или с водой
    int animal = 0;            // индекс в снимке животных
    std::uint64_t id = 0;      // постоянный идентификатор — им сортируем
    float want = 0.0f;
};

struct StepIntent {
    int animal = 0;
    int x = 0;
    int y = 0;
};

struct MateIntent {
    std::size_t cell = 0;
    int animal = 0;
    std::uint64_t id = 0;
    int species = 0;
    Sex sex = Sex::Female;
};

// Живое животное в снимке этого тика. Указатели на компоненты держать
// безопасно: за время обхода систем структура хранилища не меняется —
// создание и удаление Entity идёт только через очередь команд
// (05_Entity.md, п.5).
struct Animal {
    entt::entity entity = entt::null;
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    HerbivoreComponent* state = nullptr;
    const HerbivoreGenomeComponent* genome = nullptr;
    DesireComponent* desire = nullptr;
};

bool sortByCellThenId(const ShareIntent& a, const ShareIntent& b) {
    if (a.cell != b.cell) {
        return a.cell < b.cell;
    }
    return a.id < b.id;
}

// Какое желание сейчас гонит животное. Инерция (kDesireSwitch) — часть
// закона, а не сглаживание: желание, за которым уже пошли, держится, пока
// другое не перевесит его с заметным запасом.
Desire chooseDesire(const DesireComponent& desire, bool readyToMate) {
    const float mating = readyToMate && desire.mating >= kMateDesire ? desire.mating : 0.0f;

    Desire best = Desire::Idle;
    float bestUrgency = kDesireFloor;
    if (desire.hunger >= bestUrgency) {
        best = Desire::Food;
        bestUrgency = desire.hunger;
    }
    if (desire.thirst >= bestUrgency) {
        best = Desire::Water;
        bestUrgency = desire.thirst;
    }
    if (mating >= bestUrgency) {
        best = Desire::Mate;
        bestUrgency = mating;
    }

    float currentUrgency = 0.0f;
    switch (desire.current) {
        case Desire::Food: currentUrgency = desire.hunger; break;
        case Desire::Water: currentUrgency = desire.thirst; break;
        case Desire::Mate: currentUrgency = mating; break;
        case Desire::Idle: break;
    }
    if (desire.current != Desire::Idle && currentUrgency >= kDesireFloor &&
        bestUrgency < currentUrgency + kDesireSwitch) {
        return desire.current;
    }
    return best;
}

} // namespace

void HerbivoreSystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    auto& registry = world.registry();
    const auto& worldProperties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    const float mutationRate = worldProperties.animalMutationRate;
    const auto animalSeed = static_cast<std::uint64_t>(worldProperties.animalRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& archetypes = registry.get<const HerbivoreSpeciesComponent>(world.worldEntity()).archetypes;

    // --- 1. Снимок животных ---
    // Разреженно, а не плотным массивом на всю Область: животных десятки, а
    // клеток десятки тысяч (в отличие от почвы и травы, которых по одной на
    // клетку). Плотный массив здесь перебирал бы 10 000 клеток ради
    // тридцати существ.
    std::vector<Animal> animals;
    auto animalView =
        registry.view<HerbivoreComponent, HerbivoreGenomeComponent, DesireComponent, IdentityComponent,
                       PositionComponent>();
    for (const auto entity : animalView) {
        const auto& position = animalView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        animals.push_back(Animal{entity, animalView.get<IdentityComponent>(entity).id, position.x, position.y,
                                  &animalView.get<HerbivoreComponent>(entity),
                                  &animalView.get<HerbivoreGenomeComponent>(entity),
                                  &animalView.get<DesireComponent>(entity)});
    }
    if (animals.empty()) {
        return;
    }

    // --- 2. Снимок тайлов ---
    // А вот это — данные тайла, по одному значению на клетку, поэтому здесь
    // плотные массивы уместны (тот же приём, что в HydrologySystem и
    // PlantSystem). Снимок снимается ДО того, как хоть одно животное
    // тронулось с места: все решения этого тика принимаются по одному и
    // тому же состоянию мира, и порядок обхода на них не влияет.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> terrain(cellCount, kNullEntity);
    std::vector<float> waterAt(cellCount, 0.0f);
    std::vector<entt::entity> plantAt(cellCount, kNullEntity);
    std::vector<float> plantGrowth(cellCount, 0.0f);

    auto terrainView = registry.view<PositionComponent, SoilComponent>();
    for (const auto entity : terrainView) {
        const auto& position = terrainView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        terrain[i] = entity;
        if (const auto* water = registry.try_get<const WaterComponent>(entity)) {
            waterAt[i] = water->depth;
        }
    }

    auto plantView = registry.view<PlantComponent, PositionComponent>();
    for (const auto entity : plantView) {
        const auto& position = plantView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        plantAt[i] = entity;
        plantGrowth[i] = plantView.get<PlantComponent>(entity).growth;
    }

    std::vector<ShareIntent> bites;
    std::vector<ShareIntent> drinks;
    std::vector<StepIntent> steps;
    std::vector<MateIntent> matings;

    // --- 3. Тело и желания ---
    // Отдельным проходом от решений (п.4) намеренно: животное, выбирая
    // пару, смотрит, чего хочет сосед, — и если бы желания и решения
    // считались в одном проходе, сосед, которого EnTT хранит позже, был бы
    // ещё с прошлотиковым желанием. Порядок в памяти не может быть причиной
    // события в мире (02_CorePrinciples.md, п.12a), поэтому сначала весь
    // мир узнаёт, чего он хочет, и только потом кто-то что-то делает.
    std::vector<bool> alive(animals.size(), true);
    for (std::size_t a = 0; a < animals.size(); ++a) {
        const Animal& animal = animals[a];
        auto& state = *animal.state;
        const auto& genome = *animal.genome;
        auto& desire = *animal.desire;

        const float size = kMinSizeShare + (1.0f - kMinSizeShare) * state.growth;

        // Цена существования. Тратится всегда — стоящее на месте животное
        // тоже живёт.
        state.energy -= genome.energyUpkeep * size;
        state.water -= genome.waterUpkeep * size;
        state.age += 1.0f;

        // Пищеварение: белок не лежит в теле вечно, часть уходит навозом.
        // Отсюда и постоянная нужда есть, а не только "когда кончилась
        // энергия".
        state.dungPending += kDungRate * size;
        while (state.dungPending >= 1.0f && state.protein > 0) {
            state.dungPending -= 1.0f;
            --state.protein;
            ++state.dung;
        }
        state.dungPending = std::min(state.dungPending, 1.0f);

        // Взросление: телёнок дорастает до взрослого примерно к
        // maturityAge, но не выше того, что позволяет накопленный белок —
        // ровно как размер растения ограничен его минералами. Голодное
        // животное не растёт вовсе.
        if (state.growth < 1.0f && state.energy > 0.0f) {
            const float proteinCeiling =
                genome.proteinNeed > 0.0f ? static_cast<float>(state.protein) / genome.proteinNeed : 1.0f;
            const float ceiling = std::min(1.0f, std::max(state.growth, proteinCeiling));
            const float rate = genome.maturityAge > 1.0f ? 1.0f / genome.maturityAge : 1.0f;
            state.growth = std::clamp(state.growth + rate, 0.0f, ceiling);
        }

        if (state.energy <= 0.0f) {
            state.energy = 0.0f;
            state.stress += kStarvationStress;
        }
        if (state.water <= 0.0f) {
            state.water = 0.0f;
            state.stress += kDehydrationStress;
        }
        if (state.energy > 0.0f && state.water > 0.0f) {
            state.stress = std::max(0.0f, state.stress - kStressRelief);
        }

        // Смерть от старости или от условий. Entity исчезает не сейчас, а
        // при разрешении очереди команд (05_Entity.md, п.5). Всё вещество
        // тела возвращается в мир перегноем на той клетке, где животное
        // легло: белок, который оно накопило из травы, и не вышедший навоз.
        if (state.age >= genome.maxAge || state.stress >= 1.0f) {
            commands.enqueue([entity = animal.entity, x = animal.x, y = animal.y](World& w) {
                if (!w.registry().valid(entity)) {
                    return;
                }
                // Читаем состояние заново, а не полагаемся на снятое при
                // постановке команды: пока она ждала очереди, тик мог
                // доработать (05_Entity.md, п.5).
                int minerals = 0;
                if (const auto* body = w.registry().try_get<const HerbivoreComponent>(entity)) {
                    minerals = body->protein + body->dung;
                }
                depositHumus(w, x, y, minerals);
                w.despawn(entity);
            });
            alive[a] = false;
            continue;
        }

        // Навоз выпадает порциями на ту клетку, где животное сейчас стоит.
        if (state.dung >= kDungDrop) {
            const int dropped = state.dung;
            state.dung = 0;
            commands.enqueue([x = animal.x, y = animal.y, dropped](World& w) { depositHumus(w, x, y, dropped); });
        }

        // --- Желания ---
        // Голод и жажда не копятся сами по себе — они читаются из тела, и
        // потому падают сразу, как только животное поело или напилось. Это
        // и есть удовлетворение желания: изменившееся тело, а не списанный
        // счётчик.
        const float energyDeficit =
            genome.energyCapacity > 0.0f ? 1.0f - state.energy / genome.energyCapacity : 1.0f;
        const float proteinDeficit =
            genome.proteinNeed > 0.0f ? 1.0f - static_cast<float>(state.protein) / genome.proteinNeed : 0.0f;
        desire.hunger = std::clamp(std::max(energyDeficit, proteinDeficit), 0.0f, 1.0f);
        desire.thirst =
            std::clamp(genome.waterCapacity > 0.0f ? 1.0f - state.water / genome.waterCapacity : 1.0f, 0.0f, 1.0f);

        const bool adult = state.age >= genome.maturityAge && state.growth >= kBreedingGrowth;
        const bool content = desire.hunger < kCalmNeed && desire.thirst < kCalmNeed;
        if (adult && content) {
            // Желание пары — единственное, которого в теле не прочитать: оно
            // копится со временем у того, кому больше нечего хотеть.
            desire.mating = std::min(1.0f, desire.mating + genome.breedingUrge);
        }
        desire.current = chooseDesire(desire, adult && content);
    }

    // --- 4. Решения: что животное делает со своим желанием ---
    for (std::size_t a = 0; a < animals.size(); ++a) {
        if (!alive[a]) {
            continue;
        }
        const Animal& animal = animals[a];
        auto& state = *animal.state;
        const auto& genome = *animal.genome;
        auto& desire = *animal.desire;
        const std::size_t here = index(animal.x, animal.y);
        const float size = kMinSizeShare + (1.0f - kMinSizeShare) * state.growth;

        // Случайность собирается из seed мира, номера тика и постоянного
        // идентификатора животного (core/Random.hpp). Не из координат, как
        // у растения: клетка у животного меняется каждый шаг, а на одной
        // клетке их может стоять несколько — розыгрыш вышел бы у них
        // одинаковым (см. IdentityComponent).
        std::uint64_t random = mixSeed(animalSeed, mixSeed(tick, animal.id));

        // --- Действие ---
        // Либо желаемое рядом — и животное занимается им, никуда не идя,
        // либо оно видит, куда идти, и делает шаг. Дальше видимости
        // (perception) для животного ничего не существует: оно не может
        // хотеть того, о чём не знает (02_CorePrinciples.md, п.6).
        const int reach = std::max(1, static_cast<int>(std::lround(genome.perception)));
        bool busy = false;
        bool hasTarget = false;
        int targetX = animal.x;
        int targetY = animal.y;

        // Ближайшая клетка в пределах видимости, удовлетворяющая условию.
        // Ближайшая, а не лучшая: животное идёт к тому, что видит рядом, а
        // не выбирает оптимум по всей округе.
        //
        // Из одинаково близких выбор бросается жребием, а не достаётся
        // первой по обходу. Обход идёт с левого верхнего угла квадрата
        // видимости, и без жребия стадо на ровном лугу, где еда со всех
        // сторон одинаково близко, дружно уходило бы вверх и влево — не
        // потому, что там лучше, а потому, что цикл начинается оттуда.
        auto findNearest = [&](auto predicate) {
            float bestDistance = 0.0f;
            int ties = 0;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int nx = animal.x + dx;
                    const int ny = animal.y + dy;
                    if (!world.area().inBounds(nx, ny)) {
                        continue;
                    }
                    const float distance = static_cast<float>(dx * dx + dy * dy);
                    if (distance > static_cast<float>(reach * reach)) {
                        continue; // видимость круглая, а не квадратная
                    }
                    if (ties > 0 && distance > bestDistance) {
                        continue;
                    }
                    if (!predicate(index(nx, ny), nx, ny)) {
                        continue;
                    }
                    if (ties > 0 && distance == bestDistance) {
                        // Равноудалённая находка: занимает место прежней с
                        // вероятностью 1/N, поэтому все они равноправны.
                        ++ties;
                        if (randomUnit(random) >= 1.0f / static_cast<float>(ties)) {
                            continue;
                        }
                    } else {
                        ties = 1;
                    }
                    bestDistance = distance;
                    targetX = nx;
                    targetY = ny;
                }
            }
            return ties > 0;
        };

        switch (desire.current) {
            case Desire::Food: {
                if (plantAt[here] != entt::null && plantGrowth[here] > kMinBiteGrowth) {
                    bites.push_back(ShareIntent{here, static_cast<int>(a), animal.id, genome.biteSize * size});
                    busy = true;
                } else {
                    hasTarget = findNearest([&](std::size_t cell, int, int) {
                        return plantAt[cell] != entt::null && plantGrowth[cell] > kMinBiteGrowth;
                    });
                }
                break;
            }
            case Desire::Water: {
                // Пьёт со своей клетки или с любой соседней: травоядное
                // стоит на берегу, а не заходит в реку (в глубокую воду оно
                // и шагнуть не может, см. kWadeDepth).
                std::size_t source = cellCount;
                if (waterAt[here] > 0.0f) {
                    source = here;
                } else {
                    for (int dir = 0; dir < 8; ++dir) {
                        const int nx = animal.x + kDx8[dir];
                        const int ny = animal.y + kDy8[dir];
                        if (!world.area().inBounds(nx, ny)) {
                            continue;
                        }
                        const std::size_t j = index(nx, ny);
                        if (waterAt[j] > 0.0f) {
                            source = j;
                            break;
                        }
                    }
                }
                if (source < cellCount) {
                    drinks.push_back(ShareIntent{source, static_cast<int>(a), animal.id, kDrinkRate * size});
                    busy = true;
                } else {
                    hasTarget = findNearest([&](std::size_t cell, int, int) { return waterAt[cell] > 0.0f; });
                }
                break;
            }
            case Desire::Mate: {
                // Партнёров ищем перебором по всем животным, а не по клеткам
                // карты: их десятки, и квадрат от десятков дешевле, чем
                // просмотр круга клеток на каждого.
                float bestDistance = 0.0f;
                for (std::size_t b = 0; b < animals.size(); ++b) {
                    if (b == a || !alive[b]) {
                        continue;
                    }
                    const Animal& other = animals[b];
                    if (other.genome->species != genome.species || other.state->sex == state.sex) {
                        continue;
                    }
                    if (other.desire->current != Desire::Mate) {
                        continue; // пара нужна согласная: тот, кто занят едой, не сойдётся
                    }
                    const int dx = other.x - animal.x;
                    const int dy = other.y - animal.y;
                    const float distance = static_cast<float>(dx * dx + dy * dy);
                    if (distance > static_cast<float>(reach * reach)) {
                        continue;
                    }
                    if (hasTarget && distance >= bestDistance) {
                        continue;
                    }
                    bestDistance = distance;
                    targetX = other.x;
                    targetY = other.y;
                    hasTarget = true;
                }
                // Навстречу идёт только самец, самка при виде него ждёт на
                // месте. Это не украшение поведения, а необходимость: два
                // животных, шагающих друг к другу, каждый тик меняются
                // клетками и остаются соседями — они могут так и не
                // встретиться, продолжая всю жизнь ходить друг сквозь
                // друга. Кто именно ждёт, мир решает полом, а не жребием:
                // жребий пришлось бы бросать заново каждый тик, и пара
                // снова начала бы топтаться.
                if (hasTarget && (targetX != animal.x || targetY != animal.y) && state.sex == Sex::Female) {
                    hasTarget = false;
                    busy = true;
                }

                if (hasTarget && targetX == animal.x && targetY == animal.y) {
                    matings.push_back(
                        MateIntent{here, static_cast<int>(a), animal.id, genome.species, state.sex});
                    hasTarget = false;
                    busy = true;
                }
                break;
            }
            case Desire::Idle:
                break;
        }

        // --- Шаг ---
        // Занятое животное (ест, пьёт, сошлось с парой) с места не сходит.
        if (busy) {
            continue;
        }

        state.stepProgress += genome.speed;
        if (state.stepProgress < 1.0f) {
            continue;
        }
        state.stepProgress -= 1.0f;
        if (!hasTarget && randomUnit(random) >= kWanderChance) {
            continue; // ничего не гонит — стоит и щиплет что придётся
        }

        // Куда именно: из проходимых соседей выбирается тот, что ближе к
        // цели; без цели — случайный. В глубокую воду и на занятый
        // непроходимым объектом тайл животное не идёт, а вот на клетку с
        // травой и с другим животным — сколько угодно.
        int stepX = animal.x;
        int stepY = animal.y;
        bool stepFound = false;
        float bestScore = 0.0f;
        const int firstDir = static_cast<int>(randomUnit(random) * 8.0f) % 8;
        for (int n = 0; n < 8; ++n) {
            const int dir = (firstDir + n) % 8;
            const int nx = animal.x + kDx8[dir];
            const int ny = animal.y + kDy8[dir];
            if (!world.area().inBounds(nx, ny) || world.area().isBlocked(nx, ny)) {
                continue;
            }
            const std::size_t j = index(nx, ny);
            if (terrain[j] == entt::null || waterAt[j] > kWadeDepth) {
                continue;
            }
            if (!hasTarget) {
                stepX = nx;
                stepY = ny;
                stepFound = true;
                break; // случайное блуждание: первый же годный сосед по кругу
            }
            const float dx = static_cast<float>(targetX - nx);
            const float dy = static_cast<float>(targetY - ny);
            const float score = dx * dx + dy * dy;
            if (!stepFound || score < bestScore) {
                bestScore = score;
                stepX = nx;
                stepY = ny;
                stepFound = true;
            }
        }
        if (!stepFound) {
            continue;
        }

        state.energy = std::max(0.0f, state.energy - kStepEnergy * size);
        steps.push_back(StepIntent{static_cast<int>(a), stepX, stepY});
    }

    // --- 5. Кормёжка: один куст на всех, кто до него дотянулся ---
    // Спор делится долями: если куста на всех не хватает, каждый получает
    // свою часть. Порядок внутри клетки — по постоянному идентификатору, а
    // не по порядку в хранилище: крупицы белка целые, и кому достанется
    // последняя, решает мир, а не EnTT.
    std::sort(bites.begin(), bites.end(), sortByCellThenId);
    for (std::size_t n = 0; n < bites.size();) {
        std::size_t m = n;
        float demand = 0.0f;
        while (m < bites.size() && bites[m].cell == bites[n].cell) {
            demand += bites[m].want;
            ++m;
        }

        const entt::entity plantEntity = plantAt[bites[n].cell];
        auto* plant = registry.valid(plantEntity) ? registry.try_get<PlantComponent>(plantEntity) : nullptr;
        if (plant == nullptr || demand <= 0.0f) {
            n = m;
            continue;
        }

        const float growthBefore = plant->growth;
        const float moistureBefore = plant->moisture;
        const int mineralsBefore = plant->minerals;
        const float share = std::min(1.0f, growthBefore / demand);

        float eatenTotal = 0.0f;
        for (std::size_t k = n; k < m; ++k) {
            const float eaten = bites[k].want * share;
            if (eaten <= 0.0f) {
                continue;
            }
            auto& state = *animals[static_cast<std::size_t>(bites[k].animal)].state;
            const auto& genome = *animals[static_cast<std::size_t>(bites[k].animal)].genome;

            state.energy = std::min(genome.energyCapacity, state.energy + eaten * kEnergyPerBiomass * genome.digestion);

            // Доля куста, доставшаяся этому животному, — по ней делятся и
            // вода, и крупицы белка: съеденное растение отдаёт всё, что в
            // нём было, а не только биомассу.
            const float fraction = growthBefore > 0.0f ? eaten / growthBefore : 0.0f;
            const float sap = moistureBefore * fraction;
            state.water = std::min(genome.waterCapacity, state.water + sap * kWaterPerBiomass);
            plant->moisture = std::max(0.0f, plant->moisture - sap);

            const int proteinCap = std::max(1, static_cast<int>(std::ceil(genome.proteinNeed)));
            state.proteinPending += static_cast<float>(mineralsBefore) * fraction;
            while (state.proteinPending >= 1.0f && plant->minerals > 0) {
                state.proteinPending -= 1.0f;
                --plant->minerals;
                if (state.protein < proteinCap) {
                    ++state.protein;
                } else {
                    // Тело больше не удержит — крупица проходит насквозь и
                    // ляжет навозом там, где животное окажется.
                    ++state.dung;
                }
            }
            state.proteinPending = std::min(state.proteinPending, 1.0f);

            eatenTotal += eaten;
        }

        plant->growth = std::max(0.0f, plant->growth - eatenTotal);
        // Само по себе объедание растение не убивает: оно теряет биомассу и
        // получает стресс, а погибнуть или отрасти — решит PlantSystem по
        // своим законам на следующем тике.
        plant->stress = std::min(1.0f, plant->stress + eatenTotal * kGrazeStress);
        n = m;
    }

    // --- 6. Водопой: так же долями ---
    std::sort(drinks.begin(), drinks.end(), sortByCellThenId);
    for (std::size_t n = 0; n < drinks.size();) {
        std::size_t m = n;
        float demand = 0.0f;
        while (m < drinks.size() && drinks[m].cell == drinks[n].cell) {
            demand += drinks[m].want;
            ++m;
        }

        const std::size_t cell = drinks[n].cell;
        const entt::entity tile = terrain[cell];
        auto* water = tile != entt::null && registry.valid(tile) ? registry.try_get<WaterComponent>(tile) : nullptr;
        if (water == nullptr || demand <= 0.0f) {
            n = m;
            continue;
        }

        const float availableUnits = water->depth / kDrinkDepthPerUnit;
        const float share = std::min(1.0f, availableUnits / demand);
        float drunk = 0.0f;
        for (std::size_t k = n; k < m; ++k) {
            auto& state = *animals[static_cast<std::size_t>(drinks[k].animal)].state;
            const auto& genome = *animals[static_cast<std::size_t>(drinks[k].animal)].genome;
            const float got = std::min(drinks[k].want * share, std::max(0.0f, genome.waterCapacity - state.water));
            state.water += got;
            drunk += got;
        }
        // Выпитое уходит из клетки: воды в мире от водопоя становится
        // меньше, пусть и на малость. Исчезнет ли после этого сама вода с
        // тайла — решает HydrologySystem по своему порогу, не мы.
        water->depth = std::max(0.0f, water->depth - drunk * kDrinkDepthPerUnit);
        n = m;
    }

    // --- 7. Шаги ---
    // Применяются после всех решений: пока животные решают, мир для них
    // неподвижен, иначе сдвинувшийся сосед менял бы решение тех, кого
    // EnTT хранит позже.
    for (const auto& step : steps) {
        const Animal& animal = animals[static_cast<std::size_t>(step.animal)];
        world.moveTo(animal.entity, step.x, step.y);

        // Вытаптывание: под ногами почва становится плотнее. Это и есть
        // обратная связь стада на луг помимо поедания — по тропам к
        // водопою трава растёт хуже (habitatFit по compaction).
        const entt::entity tile = terrain[index(step.x, step.y)];
        if (tile != entt::null && registry.valid(tile)) {
            auto& soil = registry.get<SoilComponent>(tile);
            const float size =
                kMinSizeShare + (1.0f - kMinSizeShare) * animals[static_cast<std::size_t>(step.animal)].state->growth;
            soil.compaction = std::clamp(soil.compaction + kTrampleRate * size, 0.0f, 1.0f);
        }
    }

    // --- 8. Встречи: кто с кем сошёлся ---
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
        if (paired[n]) {
            continue;
        }
        std::size_t partner = matings.size();
        for (std::size_t k = n + 1; k < matings.size() && matings[k].cell == matings[n].cell; ++k) {
            if (paired[k] || matings[k].species != matings[n].species || matings[k].sex == matings[n].sex) {
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
        const Animal& mother = animals[static_cast<std::size_t>(firstIsMother ? matings[n].animal : matings[partner].animal)];
        const Animal& father = animals[static_cast<std::size_t>(firstIsMother ? matings[partner].animal : matings[n].animal)];

        const auto& motherGenome = *mother.genome;
        const auto& fatherGenome = *father.genome;
        std::uint64_t random = mixSeed(animalSeed, mixSeed(tick, mixSeed(mother.id, father.id)));

        const auto& archetype =
            (motherGenome.species >= 0 && static_cast<std::size_t>(motherGenome.species) < archetypes.size())
                ? archetypes[static_cast<std::size_t>(motherGenome.species)]
                : motherGenome;
        const HerbivoreGenomeComponent childGenome =
            crossHerbivoreGenomes(motherGenome, fatherGenome, archetype, mutationRate, random);

        HerbivoreComponent calf;
        calf.growth = kNewbornGrowth;
        calf.sex = randomUnit(random) < 0.5f ? Sex::Female : Sex::Male;

        // Телёнок появляется не из ниоткуда: и запасы, и крупица белка —
        // материнские. Ровно тот же обмен, что у растения с семенем.
        const float givenEnergy = mother.state->energy * kBirthEnergyShare;
        mother.state->energy -= givenEnergy;
        calf.energy = std::min(givenEnergy, childGenome.energyCapacity);

        const float givenWater = mother.state->water * kBirthWaterShare;
        mother.state->water -= givenWater;
        calf.water = std::min(givenWater, childGenome.waterCapacity);

        if (mother.state->protein > 0) {
            --mother.state->protein;
            calf.protein = 1;
        }
        calf.growth = childGenome.proteinNeed > 0.0f
                           ? std::min(calf.growth, static_cast<float>(calf.protein) / childGenome.proteinNeed)
                           : calf.growth;

        // Ухаживание тоже стоит энергии, но она никуда не переходит — это
        // потраченные силы, а не переданное вещество.
        father.state->energy =
            std::max(0.0f, father.state->energy - fatherGenome.energyCapacity * kCourtshipEnergyShare);

        mother.desire->mating = 0.0f;
        father.desire->mating = 0.0f;
        mother.desire->current = Desire::Idle;
        father.desire->current = Desire::Idle;

        const std::uint64_t calfId = mixSeed(random, mixSeed(mother.id, tick));
        commands.enqueue([calf, childGenome, calfId, x = mother.x, y = mother.y](World& w) {
            const auto entity = w.registry().create();
            w.registry().emplace<IdentityComponent>(entity, IdentityComponent{calfId});
            w.registry().emplace<HerbivoreComponent>(entity, calf);
            w.registry().emplace<HerbivoreGenomeComponent>(entity, childGenome);
            // Новорождённый ничего ещё не хочет — тело у него полное; чего
            // хотеть, ему скажет первый же тик (см. п.3 выше).
            w.registry().emplace<DesireComponent>(entity, DesireComponent{});
            // Проверять клетку не нужно: травоядное не занимает тайл
            // (04_WorldModel.md, п.4), поэтому телёнок всегда помещается
            // рядом с матерью.
            w.place(entity, x, y);
        });
    }
}


// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
void appendHerbivoreSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Herbivores (tick)";
    out.push_back({g, "kMinSizeShare", kMinSizeShare});
    out.push_back({g, "kEnergyPerBiomass", kEnergyPerBiomass});
    out.push_back({g, "kWaterPerBiomass", kWaterPerBiomass});
    out.push_back({g, "kStepEnergy", kStepEnergy});
    out.push_back({g, "kDrinkRate", kDrinkRate});
    out.push_back({g, "kDrinkDepthPerUnit", kDrinkDepthPerUnit});
    out.push_back({g, "kMinBiteGrowth", kMinBiteGrowth});
    out.push_back({g, "kGrazeStress", kGrazeStress});
    out.push_back({g, "kDungRate", kDungRate});
    out.push_back({g, "kDungDrop", static_cast<float>(kDungDrop)});
    out.push_back({g, "kStarvationStress", kStarvationStress});
    out.push_back({g, "kDehydrationStress", kDehydrationStress});
    out.push_back({g, "kStressRelief", kStressRelief});
    out.push_back({g, "kDesireFloor", kDesireFloor});
    out.push_back({g, "kDesireSwitch", kDesireSwitch});
    out.push_back({g, "kBreedingGrowth", kBreedingGrowth});
    out.push_back({g, "kCalmNeed", kCalmNeed});
    out.push_back({g, "kMateDesire", kMateDesire});
    out.push_back({g, "kBirthEnergyShare", kBirthEnergyShare});
    out.push_back({g, "kBirthWaterShare", kBirthWaterShare});
    out.push_back({g, "kCourtshipEnergyShare", kCourtshipEnergyShare});
    out.push_back({g, "kNewbornGrowth", kNewbornGrowth});
    out.push_back({g, "kTrampleRate", kTrampleRate});
    out.push_back({g, "kWanderChance", kWanderChance});
}

} // namespace goblins

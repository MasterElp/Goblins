#include "core/systems/PlantSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Humus.hpp"
#include "core/Scale.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SeedComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Насколько маленькое растение "меньше ест": и забор влаги, и расход, и
// добыча минералов умножаются на размер (от kMinSizeShare у проростка до
// 1 у взрослого). Без этого проросток конкурировал бы за воду наравне со
// взрослой травой и почти никогда не приживался бы на занятом лугу.
constexpr int kMinSizeShare = 300;

// Насколько взрослое растение сушит свою клетку за тик.
// SoilComponent.moisture — не запас воды, а состояние почвы, которое
// HydrologySystem тянет к своей цели (близость воды); трава эту цель
// перебивает вниз. Отсюда у клетки и есть предел, сколько травы она
// способна кормить бесконечно: куст растёт, пока подсыхание не сравнялось
// с тем, что возвращает вода. Не бесплатный полив, а конкуренция —
// с рекой, а через неё и с соседями.
constexpr int kSoilDrying = 4;

// Стресс. Ниже kVitalityFloor (доля от идеального роста) растение
// начинает страдать тем быстрее, чем хуже дела; при полном нуле
// (например, чужая почва) умирает примерно за 1/kStressGain тиков.
// Восстановление медленнее накопления: пережитая засуха не забывается
// мгновенно.
constexpr int kVitalityFloor = 500;
constexpr int kStressGain = 10;
constexpr int kStressRelief = 4;
// Затопление — отдельная, куда более быстрая смерть: трава на воде не
// растёт (не "плохо растёт"), и разлившаяся река выкашивает луг за
// десяток тиков, а не за сотню.
constexpr int kDrownStress = 100;

// Размножение. Семя может дать только достаточно выросшее и не бедствующее
// растение, и каждое семя стоит родителю части роста, крупицы минералов и
// доли запаса влаги — именно эта цена, а не отдельный "лимит потомков",
// не даёт траве залить мир за десяток тиков: после каждого семени
// растению нужно заново отрастить потраченное.
constexpr int kSeedMinGrowth = 500;
constexpr int kSeedMaxStress = 400;
// Цена семени в биомассе — заметно больше, чем "чуть-чуть": она должна
// сбрасывать растение НИЖЕ порога kSeedMinGrowth, иначе высеваться можно
// подряд каждый тик и скорость роста ни на что не влияет. С такой ценой
// частота потомства упирается ещё и в growthRate (нужно отрастить
// потраченное), то есть плодовитость приходится покупать дважды.
constexpr int kSeedGrowthCost = 450;
constexpr int kSeedlingGrowth = 20;

// Куда семя ложиться не станет: клетка суше, чем нужно потомку, чтобы
// расти хотя бы вполсилы. Один порог вместо двух прежних (пригодность
// почвы и запас влаги) — потому что и причина осталась одна. Это не
// гарантия выживания, только отказ от заведомо мёртвых мест. Тот же
// порог использует и первичное расселение (kSeedingMinSupply в
// GrassSeeding.cpp).
constexpr int kSeedMinSupply = 500;

// Семя, оставленное в своей же клетке (когда ронять потомка некуда),
// лежит не вечно: сперва спит положенный геному срок
// (PlantGenomeComponent::seedDormancy), потом столько тиков сторожит
// клетку, а не дождавшись — гниёт, и его крупица уходит в перегной.
// Окно ожидания — общее для всех, а не черта генома: иначе плата за
// "долго лежащее семя" уходила бы в тот же бюджет, что и срок покоя, и
// два числа тянули бы одну и ту же верёвку с разных концов. Величина
// соразмерна жизни травы (сотни тиков): семя переживает родителя, но не
// несколько поколений подряд — семенной банк в мире есть, вечного
// семенного банка нет.
constexpr int kSeedWaitTicks = 400;

// Намерение посеять: собирается при обходе растений, исполняется после
// него. Отдельный шаг нужен потому, что на одну свободную клетку могут
// нацелиться сразу несколько соседей, и решать, кто её займёт, порядком
// обхода Entity нельзя — результат тика не должен зависеть от того, в
// каком порядке EnTT хранит растения (как и в HydrologySystem, где всё
// считается из снимка в накопители). Спор выигрывает наибольший
// priority — число, посчитанное из seed мира, номера тика и обеих клеток,
// то есть одинаковое при любом порядке обхода и разное у разных пар
// соседей.
struct SeedIntent {
    entt::entity parent;
    std::size_t targetCell;
    std::uint64_t priority;
    PlantGenomeComponent child;
};

} // namespace

void PlantSystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    auto& registry = world.registry();
    const auto& worldProperties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    // Настройка мира целая (тысячные доли вложения), а расклад бюджета
    // черт дробный: перевод один раз здесь, на границе (core/Scale.hpp).
    const float mutationRate = static_cast<float>(worldProperties.plantMutationRate) / kFull;
    const int humusDecayPeriod = std::max(1, worldProperties.humusDecayPeriod);
    const auto plantSeed = static_cast<std::uint64_t>(worldProperties.plantRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& archetypes = registry.get<const PlantSpeciesComponent>(world.worldEntity()).archetypes;

    // --- 1. Снимок тайлов: где почва, где вода, где уже стоит растение ---
    // Тот же приём, что и в HydrologySystem: сначала плоские массивы по
    // Области, потом расчёт — иначе на каждое соседство пришлось бы
    // перебирать список Entity клетки.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> terrain(cellCount, kNullEntity);
    // Глубина воды, а не флаг "вода есть": переносимая глубина у каждого
    // вида своя (PlantGenomeComponent::waterTolerance), поэтому решение
    // "тонет или нет" принимается для каждого растения отдельно, ниже.
    std::vector<int> waterAt(cellCount, 0);
    std::vector<unsigned char> occupied(cellCount, 0);
    // Почва СОСЕДНИХ клеток — именно снимком, снятым до того, как хоть
    // одно растение начало пить и есть. Растение правит почву только своей
    // клетки (на тайле оно одно), а вот выбирая, куда уронить семя, оно
    // смотрит на соседей — и если смотреть на живые компоненты, то видно
    // уже объеденную теми соседями почву, которых EnTT просто хранит
    // раньше в памяти. Порядок хранения — деталь реализации, а не событие
    // мира: он давал бы устойчивое преимущество одним растениям перед
    // другими, и это было бы не случайностью, а перекосом.
    std::vector<SoilComponent> soilAt(cellCount);

    auto terrainView = registry.view<PositionComponent, SoilComponent>();
    for (const auto entity : terrainView) {
        const auto& position = terrainView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        terrain[i] = entity;
        soilAt[i] = terrainView.get<SoilComponent>(entity);
        if (const auto* water = registry.try_get<const WaterComponent>(entity)) {
            waterAt[i] = water->depth;
        }
    }

    for (const auto entity : registry.view<PlantComponent, PositionComponent>()) {
        const auto& position = registry.get<const PositionComponent>(entity);
        if (world.area().inBounds(position.x, position.y)) {
            occupied[index(position.x, position.y)] = 1;
        }
    }

    // Где уже лежит семя. Отдельно от occupied: семя не занимает клетку
    // для растения (оно лежит в земле под ним, обычно как раз под своим
    // родителем) — но второму семени места в клетке нет.
    std::vector<unsigned char> seeded(cellCount, 0);
    for (const auto entity : registry.view<SeedComponent, PositionComponent>()) {
        const auto& position = registry.get<const PositionComponent>(entity);
        if (world.area().inBounds(position.x, position.y)) {
            seeded[index(position.x, position.y)] = 1;
        }
    }

    // --- 2. Жизнь растений ---
    // Намерения посеять копятся здесь и исполняются ниже, после обхода
    // (см. SeedIntent): занятость клеток при выборе места берётся из
    // снимка выше и в ходе обхода не меняется, поэтому кто куда целится
    // не зависит от порядка обхода Entity.
    std::vector<SeedIntent> seedIntents;
    auto plants = registry.view<PlantComponent, PlantGenomeComponent, PositionComponent>();
    for (const auto entity : plants) {
        auto& plant = plants.get<PlantComponent>(entity);
        const auto& genome = plants.get<PlantGenomeComponent>(entity);
        const auto& position = plants.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        if (terrain[i] == entt::null) {
            continue;
        }
        auto& soil = registry.get<SoilComponent>(terrain[i]);

        const int size = kMinSizeShare + (kFull - kMinSizeShare) * plant.growth / kFull;
        // Тонет растение или нет, решает не факт наличия воды, а её
        // глубина против переносимой этим геномом (water_tolerance).
        // Мокрая земля и мелкая лужа после дождя росту не мешают вовсе;
        // настоящее затопление — клетка непригодна (пригодность 0), и
        // дальше растение добьёт kDrownStress.
        const bool drowning = waterAt[i] > genome.waterTolerance;

        // Обеспеченность влагой — единственное, что решает, хорошо ли
        // растению на этом месте. Отдельной "пригодности почвы" по
        // каменистости и утоптанности больше нет: камень и так суше
        // (kRockMoistureReduction, core/Moisture.hpp), и голые горы
        // получаются от той же причины, что и всё остальное здесь.
        //
        // Запаса влаги в теле тоже нет — растение пьёт из клетки прямо
        // сейчас, а не живёт накопленным. Засуху переживает не тот, у кого
        // больше запас, а тот, кому изначально нужно меньше.
        const int supply =
            genome.moistureNeed > 0 ? std::min(kFull, soil.moisture * kFull / genome.moistureNeed) : kFull;

        // Растущее растение сушит свою клетку — отсюда и предел, сколько
        // травы клетка способна кормить бесконечно: HydrologySystem тянет
        // влажность обратно к цели, и куст встаёт там, где эти две
        // скорости сравнялись.
        soil.moisture = std::clamp(soil.moisture - kSoilDrying * size / kFull, 0, kFull);

        // Минералы: крупица уходит из почвы в растение тогда, когда рост в
        // неё упёрся. Больше mineralNeed растение не запасает — иначе одна
        // старая трава высосала бы всю клетку и вернула бы туда перегноем
        // гораздо больше, чем ей когда-либо было нужно.
        const int mineralCap = std::max(1, genome.mineralNeed);

        // Рост: скорость * обеспеченность влагой, но не выше того, что
        // позволяют накопленные минералы. Потолок никогда не уменьшает уже
        // достигнутый размер — отданная семени крупица минералов не должна
        // заставлять взрослое растение съёжиться.
        //
        // Скорость роста дробна по существу (скорость генома, ослабленная
        // влагой), поэтому неделящийся остаток остаётся в growthProgress —
        // целым, а не дробью. Без него растение при малой обеспеченности
        // не росло бы вовсе: целое деление отбросило бы весь прирост.
        const int vitality = drowning ? 0 : supply;
        plant.growthProgress += genome.growthRate * vitality;
        const int wanted = std::min(kFull, plant.growth + plant.growthProgress / kFull);
        plant.growthProgress %= kFull;

        // Сколько крупиц нужно, чтобы дорасти до желаемого. Пока их нет в
        // почве, растение просто не растёт — и ждёт, а не выгребает клетку
        // про запас: минералы туда ещё принесёт течение или перегной.
        const int wantedMinerals =
            genome.mineralNeed > 0 ? std::min(mineralCap, (wanted * genome.mineralNeed + kFull - 1) / kFull) : 0;
        while (plant.minerals < wantedMinerals && soil.minerals > 0) {
            --soil.minerals;
            ++plant.minerals;
        }

        const int mineralCeiling = genome.mineralNeed > 0 ? plant.minerals * kFull / genome.mineralNeed : kFull;
        const int ceiling = std::min(kFull, std::max(plant.growth, mineralCeiling));
        plant.growth = std::clamp(wanted, 0, ceiling);

        if (drowning) {
            plant.stress += kDrownStress;
        } else if (vitality < kVitalityFloor) {
            plant.stress += kStressGain * (kVitalityFloor - vitality) / kVitalityFloor;
        } else {
            plant.stress = std::max(0, plant.stress - kStressRelief);
        }

        plant.age += 1;

        // Смерть от старости или от условий. Entity исчезает не сейчас, а
        // при разрешении очереди команд (05_Entity.md, п.5) — до конца
        // тика клетка считается занятой, и никто не посеет туда семя.
        if (plant.age >= genome.maxAge || plant.stress >= kFull) {
            commands.enqueue([entity, x = position.x, y = position.y](World& w) {
                if (!w.registry().valid(entity)) {
                    return;
                }
                // Сколько минералов уходит в перегной, читаем сейчас, а не
                // берём снятым при постановке команды: пока она ждала
                // очереди, тик успел доработать — например, растение
                // объело травоядное (AnimalSystem идёт следом) и часть
                // крупиц уже ушла в него. Со снятым числом эти крупицы
                // легли бы в перегной ВТОРОЙ раз.
                int minerals = 0;
                if (const auto* dying = w.registry().try_get<const PlantComponent>(entity)) {
                    minerals = dying->minerals;
                }
                depositHumus(w, x, y, minerals);
                w.despawn(entity);
            });
            continue;
        }

        // --- Размножение ---
        if (plant.age < genome.maturityAge || plant.growth < kSeedMinGrowth || plant.stress > kSeedMaxStress ||
            plant.minerals < 1) {
            continue;
        }

        // Случайность собирается из seed мира, номера тика и координат
        // клетки: система не хранит генератор между тиками (05_Entity.md,
        // п.3), а исход не зависит от порядка обхода Entity. Второе — не
        // про повторяемость прогонов (мир недетерминирован,
        // 02_CorePrinciples.md, п.12a), а про то, что место в памяти не
        // должно быть причиной события в мире. Координаты, а не
        // идентификатор Entity, потому что при загрузке мира
        // идентификаторы выдаются заново, а координаты — те же.
        std::uint64_t random = mixSeed(plantSeed, mixSeed(tick, static_cast<std::uint64_t>(i)));
        // Шанс — тысячные за тик, ослабленные развитостью: недоросшее
        // растение сеет реже. Бросок целый (randomBelow), поэтому и
        // сравнивать есть с чем без всякого перевода в доли.
        if (static_cast<int>(randomBelow(random, static_cast<std::uint64_t>(kFull) * kFull)) >=
            genome.seedChance * plant.growth) {
            continue;
        }

        const auto& archetype =
            (genome.species >= 0 && static_cast<std::size_t>(genome.species) < archetypes.size())
                ? archetypes[static_cast<std::size_t>(genome.species)]
                : genome;
        const PlantGenomeComponent child =
            mutateGenome(genome, archetype, mutationRate, mixSeed(random, static_cast<std::uint64_t>(i)));

        // Клетка для потомка выбирается среди восьми соседей: только те,
        // что существуют, свободны от другого растения, не залиты водой,
        // не заняты непроходимым объектом и подходят ИМЕННО ЭТОМУ геному
        // (у потомка он уже свой, слегка отличный от родительского).
        // Выбор — рулеткой по пригодности: чаще, но не всегда, семя
        // ложится в лучшую из соседних клеток, поэтому луг сам ползёт в
        // сторону подходящей почвы, но не превращается в ровный
        // наступающий фронт.
        std::size_t candidates[8];
        int weights[8];
        int candidateCount = 0;
        int totalWeight = 0;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = position.x + kDx8[dir];
            const int ny = position.y + kDy8[dir];
            if (!world.area().inBounds(nx, ny)) {
                continue;
            }
            const std::size_t j = index(nx, ny);
            // Переносимость воды берём у потомка: сеять есть смысл туда,
            // где сможет жить именно он, а его геном уже помутирован.
            if (occupied[j] != 0 || waterAt[j] > child.waterTolerance || terrain[j] == entt::null ||
                world.area().isBlocked(nx, ny)) {
                continue;
            }
            // Почва соседа — из снимка начала тика (см. soilAt выше), а
            // не живая: иначе увиденное зависело бы от того, кого EnTT
            // хранит раньше.
            const auto& targetSoil = soilAt[j];
            if (targetSoil.minerals <= 0) {
                continue; // расти не на чем: своей крупицы семечку хватит лишь на первые проценты роста
            }
            // Пригодность клетки для потомка — та же обеспеченность
            // влагой, по которой живёт и сам родитель (см. supply выше).
            // Отдельной функции пригодности почвы больше нет, и порогов
            // при ней тоже: клетка суше, чем нужно потомку в половину
            // силы, — не место для семени.
            const int targetSupply =
                child.moistureNeed > 0 ? std::min(kFull, targetSoil.moisture * kFull / child.moistureNeed) : kFull;
            if (targetSupply < kSeedMinSupply) {
                continue;
            }
            candidates[candidateCount] = j;
            weights[candidateCount] = targetSupply;
            totalWeight += targetSupply;
            ++candidateCount;
        }
        // --- Ронять некуда: семя остаётся в своей клетке ---
        // Все восемь соседей заняты, залиты или не годятся потомку. Семя
        // от этого не пропадает: оно ложится в ту же клетку, где стоит
        // родитель, и ждёт там своего часа (SeedComponent) — пока
        // родитель не умрёт от старости, не будет съеден или не сгинет от
        // стресса. Именно так вид держит занятое место за собой: на
        // заросшем лугу свободных клеток нет вовсе, и без семян
        // освободившуюся клетку занимал бы только тот, кто случайно
        // окажется рядом в нужный тик.
        if (candidateCount == 0 || totalWeight <= 0) {
            if (seeded[i] != 0) {
                continue; // одна клетка — одно семя; бросок пропал впустую, но и цена не уплачена
            }
            seeded[i] = 1;

            // Цена та же, что и у семени, брошенного в соседнюю клетку:
            // семя есть семя, и растению всё равно, куда оно упало.
            plant.growth = std::max(0, plant.growth - kSeedGrowthCost);
            --plant.minerals;

            SeedComponent seed;
            seed.minerals = 1; // та самая крупица родителя: минералы не появляются из ниоткуда

            commands.enqueue([child, seed, x = position.x, y = position.y](World& w) {
                const auto entity = w.registry().create();
                w.registry().emplace<SeedComponent>(entity, seed);
                w.registry().emplace<PlantGenomeComponent>(entity, child);
                w.place(entity, x, y);
            });
            continue;
        }

        int roll = static_cast<int>(randomBelow(random, static_cast<std::uint64_t>(totalWeight)));
        int picked = candidateCount - 1;
        for (int c = 0; c < candidateCount; ++c) {
            roll -= weights[c];
            if (roll < 0) {
                picked = c;
                break;
            }
        }
        const std::size_t targetCell = candidates[picked];

        // Сеем не здесь: клетку могли выбрать и другие соседи, а решать
        // спор порядком обхода нельзя (см. SeedIntent). Цену семени
        // родитель платит тоже там — проигравший спор не должен платить
        // за потомка, которого не будет.
        seedIntents.push_back(SeedIntent{
            entity, targetCell,
            mixSeed(plantSeed, mixSeed(tick, targetCell * 1000003ull + static_cast<std::uint64_t>(i))), child});
    }

    // --- 2b. Разрешение споров за клетку и собственно посев ---
    // Сортируем по клетке, а внутри клетки — по убыванию priority, и
    // берём по одному намерению на клетку: результат один и тот же при
    // любом порядке обхода растений выше.
    std::sort(seedIntents.begin(), seedIntents.end(), [](const SeedIntent& a, const SeedIntent& b) {
        if (a.targetCell != b.targetCell) {
            return a.targetCell < b.targetCell;
        }
        return a.priority > b.priority;
    });

    for (std::size_t n = 0; n < seedIntents.size(); ++n) {
        if (n > 0 && seedIntents[n].targetCell == seedIntents[n - 1].targetCell) {
            continue; // клетку уже забрал сосед с большим priority
        }
        const SeedIntent& intent = seedIntents[n];
        const PlantGenomeComponent& child = intent.child;
        const int targetX = static_cast<int>(intent.targetCell % static_cast<std::size_t>(width));
        const int targetY = static_cast<int>(intent.targetCell / static_cast<std::size_t>(width));

        // Цена семени для родителя.
        auto& parent = registry.get<PlantComponent>(intent.parent);
        parent.growth = std::max(0, parent.growth - kSeedGrowthCost);
        --parent.minerals;

        PlantComponent seedling;
        seedling.growth = kSeedlingGrowth;
        seedling.minerals = 1; // та самая крупица, отданная родителем: минералы не появляются из ниоткуда

        commands.enqueue([child, seedling, targetX, targetY](World& w) {
            // Пока команда ждала своей очереди, тик успел доработать:
            // например, воду на клетке мог добавить HydrologySystem
            // (его команды встали в очередь раньше). Команда не имеет
            // права полагаться на состояние, снятое до её постановки, —
            // проверяем клетку заново.
            bool blocked = w.area().isBlocked(targetX, targetY);
            for (const auto tile : w.area().cellAt(targetX, targetY).entities) {
                if (w.registry().all_of<PlantComponent>(tile) || w.registry().all_of<WaterComponent>(tile)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                // Семя не проросло — крупица минералов, которую отдал
                // ему родитель, остаётся в почве той клетки, куда семя
                // упало. Не потому, что вещество обязано сохраняться (мир
                // открыт, 02_CorePrinciples.md, п.12b), а потому, что
                // упавшему семени просто некуда деться, кроме земли.
                for (const auto tile : w.area().cellAt(targetX, targetY).entities) {
                    if (auto* soilComponent = w.registry().try_get<SoilComponent>(tile)) {
                        soilComponent->minerals += seedling.minerals;
                        break;
                    }
                }
                return;
            }

            const auto entity = w.registry().create();
            w.registry().emplace<PlantComponent>(entity, seedling);
            w.registry().emplace<PlantGenomeComponent>(entity, child);
            w.place(entity, targetX, targetY);
        });
    }

    // --- 2c. Семена: покой, прорастание, гниение ---
    // Семя ничего не делает: не растёт, не пьёт, не тянет минералы из
    // почвы и не подлежит объеданию (у него нет PlantComponent). Оно
    // только считает тики и смотрит, не освободилась ли клетка.
    auto seeds = registry.view<SeedComponent, PlantGenomeComponent, PositionComponent>();
    for (const auto entity : seeds) {
        auto& seed = seeds.get<SeedComponent>(entity);
        const auto& genome = seeds.get<PlantGenomeComponent>(entity);
        const auto& position = seeds.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);

        seed.age += 1;

        // Не дождалось. Крупица минералов уходит в перегной той же
        // клетки — туда же, куда её вернуло бы и проросшее из этого семени
        // растение, только без промежуточной жизни.
        if (seed.age >= genome.seedDormancy + kSeedWaitTicks) {
            commands.enqueue([entity, x = position.x, y = position.y](World& w) {
                if (!w.registry().valid(entity)) {
                    return;
                }
                int minerals = 0;
                if (const auto* rotting = w.registry().try_get<const SeedComponent>(entity)) {
                    minerals = rotting->minerals;
                }
                depositHumus(w, x, y, minerals);
                w.despawn(entity);
            });
            continue;
        }

        if (seed.age < genome.seedDormancy) {
            continue; // ещё спит: срок покоя — черта генома (seed_dormancy)
        }

        // Клетка ещё занята растением (обычно тем самым родителем) — семя
        // просто ждёт дальше. Растение, умершее на этом же тике, в снимке
        // occupied всё ещё стоит: его Entity исчезнет только при
        // разрешении очереди команд, поэтому семя займёт клетку тиком
        // позже, а не в тот же миг.
        if (occupied[i] != 0 || waterAt[i] > genome.waterTolerance || terrain[i] == entt::null ||
            world.area().isBlocked(position.x, position.y)) {
            continue;
        }

        PlantComponent seedling;
        seedling.growth = kSeedlingGrowth;
        seedling.minerals = seed.minerals;

        commands.enqueue([entity, genome, seedling, x = position.x, y = position.y](World& w) {
            if (!w.registry().valid(entity)) {
                return;
            }
            // Клетку могли занять, пока команда ждала очереди: например,
            // семя соседнего растения легло сюда тем же тиком (его команда
            // встала в очередь раньше). Проверяем заново — иначе на клетке
            // оказалось бы два растения.
            bool blocked = w.area().isBlocked(x, y);
            for (const auto tile : w.area().cellAt(x, y).entities) {
                if (w.registry().all_of<PlantComponent>(tile)) {
                    blocked = true;
                    break;
                }
                const auto* water = w.registry().try_get<const WaterComponent>(tile);
                if (water != nullptr && water->depth > genome.waterTolerance) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                return; // семя остаётся лежать и попробует на следующем тике
            }

            // Семя не порождает растение, а САМО им становится: тот же
            // Entity, тот же геном, то же место — меняется лишь набор
            // компонентов (02_CorePrinciples.md, п.3). Отдельная сущность
            // "проросток" миру не нужна.
            w.registry().emplace<PlantComponent>(entity, seedling);
            w.registry().remove<SeedComponent>(entity);
        });
    }

    // --- 3. Перегной: разложение и возврат минералов в почву ---
    // Ровно те крупицы, что растение при жизни вынуло из этой клетки,
    // возвращаются в неё же — по одной раз в humusDecayPeriod тиков. Пока
    // перегной лежит, клетка постепенно становится плодороднее; когда
    // возвращать нечего, компонент снимается (02_CorePrinciples.md, п.3:
    // нет перегноя — нет и компонента).
    //
    // Срок, а не дробная скорость: "0.02 крупицы за тик" требовало поля
    // pending в самом перегное — только затем, чтобы дробь дожила до
    // целого. Отсчёт сдвинут на номер тайла, иначе весь перегной мира
    // отдавал бы свою крупицу одним и тем же тиком, а между этими тиками
    // почва не менялась бы вовсе.
    auto humusView = registry.view<HumusComponent, SoilComponent, PositionComponent>();
    for (const auto entity : humusView) {
        auto& humus = humusView.get<HumusComponent>(entity);
        auto& soil = humusView.get<SoilComponent>(entity);
        const auto& humusPosition = humusView.get<PositionComponent>(entity);

        const auto tile = static_cast<std::uint64_t>(index(humusPosition.x, humusPosition.y));
        if (humus.minerals > 0 && (tick + tile) % static_cast<std::uint64_t>(humusDecayPeriod) == 0) {
            --humus.minerals;
            ++soil.minerals;
        }

        if (humus.minerals <= 0) {
            commands.enqueue([entity](World& w) {
                // Проверяем заново, а не полагаемся на снятое выше
                // состояние: между постановкой команды и её выполнением
                // на этом же тайле могло умереть растение (команда смерти
                // встала в очередь раньше), и в опустевший перегной уже
                // легли его минералы — снять компонент сейчас значило бы
                // выбросить их из мира.
                auto* humusComponent = w.registry().try_get<HumusComponent>(entity);
                if (humusComponent != nullptr && humusComponent->minerals <= 0) {
                    w.registry().remove<HumusComponent>(entity);
                }
            });
        }
    }
}


// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
void appendPlantSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Plants (tick)";
    out.push_back({g, "kMinSizeShare", kMinSizeShare});
    out.push_back({g, "kSoilDrying", kSoilDrying});
    out.push_back({g, "kVitalityFloor", kVitalityFloor});
    out.push_back({g, "kStressGain", kStressGain});
    out.push_back({g, "kStressRelief", kStressRelief});
    out.push_back({g, "kDrownStress", kDrownStress});
    out.push_back({g, "kSeedMinGrowth", kSeedMinGrowth});
    out.push_back({g, "kSeedMaxStress", kSeedMaxStress});
    out.push_back({g, "kSeedGrowthCost", kSeedGrowthCost});
    out.push_back({g, "kSeedlingGrowth", kSeedlingGrowth});
    out.push_back({g, "kSeedMinSupply", kSeedMinSupply});
    out.push_back({g, "kSeedWaitTicks", kSeedWaitTicks});
}

} // namespace goblins

#include "core/systems/PlantSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/components/HumusComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/PlantGenetics.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Насколько маленькое растение "меньше ест": и забор влаги, и расход, и
// добыча минералов умножаются на размер (от kMinSizeShare у проростка до
// 1 у взрослого). Без этого проросток конкурировал бы за воду наравне со
// взрослой травой и почти никогда не приживался бы на занятом лугу.
constexpr float kMinSizeShare = 0.3f;

// Какая часть выпитой растением влаги считается высохшей ИЗ клетки.
// SoilComponent.moisture — не запас воды, а состояние почвы, которое
// HydrologySystem тянет к своей цели (близость воды), поэтому "сколько
// выпило" и "насколько просела влажность" — разные величины, и переводной
// коэффициент здесь неизбежен. Из-за него у клетки есть предел, сколько
// влаги она способна отдавать бесконечно: чем гуще трава, тем суше почва
// под ней — так и получается конкуренция за воду, а не бесплатный полив.
constexpr float kSoilDrying = 0.5f;

// Стресс. Ниже kVitalityFloor (доля от идеального роста) растение
// начинает страдать тем быстрее, чем хуже дела; при полном нуле
// (например, чужая почва) умирает примерно за 1/kStressGain тиков.
// Восстановление медленнее накопления: пережитая засуха не забывается
// мгновенно.
constexpr float kVitalityFloor = 0.5f;
constexpr float kStressGain = 0.01f;
constexpr float kStressRelief = 0.004f;
// Затопление — отдельная, куда более быстрая смерть: трава на воде не
// растёт (не "плохо растёт"), и разлившаяся река выкашивает луг за
// десяток тиков, а не за сотню.
constexpr float kDrownStress = 0.1f;

// Размножение. Семя может дать только достаточно выросшее и не бедствующее
// растение, и каждое семя стоит родителю части роста, крупицы минералов и
// доли запаса влаги — именно эта цена, а не отдельный "лимит потомков",
// не даёт траве залить мир за десяток тиков: после каждого семени
// растению нужно заново отрастить потраченное.
constexpr float kSeedMinGrowth = 0.5f;
constexpr float kSeedMaxStress = 0.4f;
// Цена семени в биомассе — заметно больше, чем "чуть-чуть": она должна
// сбрасывать растение НИЖЕ порога kSeedMinGrowth, иначе высеваться можно
// подряд каждый тик и скорость роста ни на что не влияет. С такой ценой
// частота потомства упирается ещё и в growthRate (нужно отрастить
// потраченное), то есть плодовитость приходится покупать дважды.
constexpr float kSeedGrowthCost = 0.45f;
constexpr float kSeedMoistureShare = 0.25f;
constexpr float kSeedlingGrowth = 0.02f;

// Куда семя ложиться не станет: клетка, где потомку совсем не подходит
// почва (тот же порог, что и при генерации, — kSeedingMinFit в
// GrassSeeding.cpp), или где влаги заведомо не хватит даже на половину
// его потребности. Это не гарантия выживания — только отказ от заведомо
// мёртвых мест.
constexpr float kSeedMinFit = 0.25f;
constexpr float kSeedMoistureMargin = 0.5f;

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
    const float mutationRate = worldProperties.plantMutationRate;
    const float humusDecayRate = worldProperties.humusDecayRate;
    const auto plantSeed = static_cast<std::uint64_t>(worldProperties.plantRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& archetypes = registry.get<const PlantSpeciesComponent>(world.worldEntity()).archetypes;

    // --- 1. Снимок тайлов: где почва, где вода, где уже стоит растение ---
    // Тот же приём, что и в HydrologySystem: сначала плоские массивы по
    // Области, потом расчёт — иначе на каждое соседство пришлось бы
    // перебирать список Entity клетки.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> terrain(cellCount, kNullEntity);
    std::vector<unsigned char> flooded(cellCount, 0);
    std::vector<unsigned char> occupied(cellCount, 0);

    for (const auto entity : registry.view<PositionComponent, SoilComponent>()) {
        const auto& position = registry.get<const PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        terrain[i] = entity;
        flooded[i] = registry.all_of<WaterComponent>(entity) ? 1 : 0;
    }

    for (const auto entity : registry.view<PlantComponent, PositionComponent>()) {
        const auto& position = registry.get<const PositionComponent>(entity);
        if (world.area().inBounds(position.x, position.y)) {
            occupied[index(position.x, position.y)] = 1;
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

        const float size = kMinSizeShare + (1.0f - kMinSizeShare) * plant.growth;
        // Затопленная клетка не "плохая", а непригодная: трава на воде не
        // растёт вообще (пригодность 0), дальше её добьёт kDrownStress.
        const float fit = flooded[i] != 0 ? 0.0f : habitatFit(genome, soil);

        // Влага: сколько удалось выпить из своей клетки (из сухой почвы
        // нечего брать — забор пропорционален её влажности), столько и
        // прибавилось к собственному запасу; клетка от этого сохнет.
        const float intake = std::min(genome.moistureUptake * soil.moisture * size,
                                       std::max(0.0f, genome.moistureCapacity - plant.moisture));
        plant.moisture += intake;
        soil.moisture = std::clamp(soil.moisture - intake * kSoilDrying, 0.0f, 1.0f);

        // Расход идёт из запаса, а не напрямую из почвы: в этом и смысл
        // запаса — пока он есть, засуха росту не мешает.
        const float need = genome.moistureNeed * size;
        const float spent = std::min(plant.moisture, need);
        plant.moisture -= spent;
        const float supply = need > 0.0f ? spent / need : 1.0f;

        // Минералы: доля копится в mineralPending, целая крупица уходит из
        // почвы в растение. Больше mineralNeed растение не запасает — иначе
        // одна старая трава высосала бы всю клетку и вернула бы туда
        // перегноем гораздо больше, чем ей когда-либо было нужно.
        const int mineralCap = std::max(1, static_cast<int>(std::ceil(genome.mineralNeed)));
        plant.mineralPending += genome.mineralUptake * size;
        while (plant.mineralPending >= 1.0f && plant.minerals < mineralCap && soil.minerals > 0) {
            plant.mineralPending -= 1.0f;
            --soil.minerals;
            ++plant.minerals;
        }
        // Про запас "жажда минералов" не копится: на обеднённой клетке
        // растение ждёт, а не выгребает всё разом, как только минералы
        // туда принесёт HydrologySystem или разложившийся перегной.
        plant.mineralPending = std::min(plant.mineralPending, 1.0f);

        // Рост: скорость * пригодность клетки * обеспеченность влагой, но
        // не выше того, что позволяют накопленные минералы. Потолок
        // никогда не уменьшает уже достигнутый размер — отданная семени
        // крупица минералов не должна заставлять взрослое растение
        // съёжиться.
        const float vitality = fit * supply;
        const float mineralCeiling =
            genome.mineralNeed > 0.0f ? static_cast<float>(plant.minerals) / genome.mineralNeed : 1.0f;
        const float ceiling = std::min(1.0f, std::max(plant.growth, mineralCeiling));
        plant.growth = std::clamp(plant.growth + genome.growthRate * vitality, 0.0f, ceiling);

        if (flooded[i] != 0) {
            plant.stress += kDrownStress;
        } else if (vitality < kVitalityFloor) {
            plant.stress += kStressGain * (1.0f - vitality / kVitalityFloor);
        } else {
            plant.stress = std::max(0.0f, plant.stress - kStressRelief);
        }

        plant.age += 1.0f;

        // Смерть от старости или от условий. Entity исчезает не сейчас, а
        // при разрешении очереди команд (05_Entity.md, п.5) — до конца
        // тика клетка считается занятой, и никто не посеет туда семя.
        if (plant.age >= genome.maxAge || plant.stress >= 1.0f) {
            commands.enqueue([entity, x = position.x, y = position.y, minerals = plant.minerals](World& w) {
                if (!w.registry().valid(entity)) {
                    return;
                }
                // Перегной — не отдельный объект, а состояние тайла: он
                // ложится на тот же терраформирующий Entity, что и почва
                // с водой (см. HumusComponent). Если перегной там уже
                // есть, минералы просто складываются.
                if (minerals > 0) {
                    for (const auto tile : w.area().cellAt(x, y).entities) {
                        if (!w.registry().all_of<SoilComponent>(tile)) {
                            continue;
                        }
                        if (auto* humus = w.registry().try_get<HumusComponent>(tile)) {
                            humus->minerals += minerals;
                        } else {
                            w.registry().emplace<HumusComponent>(tile, HumusComponent{minerals, 0.0f});
                        }
                        break;
                    }
                }
                w.area().remove(entity, x, y);
                w.registry().destroy(entity);
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
        // п.3), а результат при этом воспроизводим и не зависит от порядка
        // обхода Entity. Координаты, а не идентификатор Entity, потому что
        // при загрузке мира идентификаторы выдаются заново, а координаты —
        // те же.
        std::uint64_t random = mixSeed(plantSeed, mixSeed(tick, static_cast<std::uint64_t>(i)));
        if (randomUnit(random) >= genome.seedChance * plant.growth) {
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
        // сторону подходящей почвы, но не превращается в строгий
        // детерминированный фронт.
        std::size_t candidates[8];
        float weights[8];
        int candidateCount = 0;
        float totalWeight = 0.0f;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = position.x + kDx8[dir];
            const int ny = position.y + kDy8[dir];
            if (!world.area().inBounds(nx, ny)) {
                continue;
            }
            const std::size_t j = index(nx, ny);
            if (occupied[j] != 0 || flooded[j] != 0 || terrain[j] == entt::null || world.area().isBlocked(nx, ny)) {
                continue;
            }
            const auto& targetSoil = registry.get<const SoilComponent>(terrain[j]);
            if (targetSoil.minerals <= 0) {
                continue; // расти не на чем: своей крупицы семечку хватит лишь на первые проценты роста
            }
            const float targetFit = habitatFit(child, targetSoil);
            if (targetFit < kSeedMinFit) {
                continue;
            }
            if (targetSoil.moisture * child.moistureUptake < child.moistureNeed * kSeedMoistureMargin) {
                continue;
            }
            candidates[candidateCount] = j;
            weights[candidateCount] = targetFit;
            totalWeight += targetFit;
            ++candidateCount;
        }
        if (candidateCount == 0 || totalWeight <= 0.0f) {
            continue;
        }

        float roll = randomUnit(random) * totalWeight;
        int picked = candidateCount - 1;
        for (int c = 0; c < candidateCount; ++c) {
            roll -= weights[c];
            if (roll <= 0.0f) {
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
        parent.growth = std::max(0.0f, parent.growth - kSeedGrowthCost);
        --parent.minerals;
        const float childMoisture = std::min(parent.moisture * kSeedMoistureShare, child.moistureCapacity);
        parent.moisture -= childMoisture;

        PlantComponent seedling;
        seedling.growth = kSeedlingGrowth;
        seedling.moisture = childMoisture;
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
                // Семя не проросло — но крупица минералов, которую отдал
                // ему родитель, не должна пропасть из мира: она остаётся
                // в почве той клетки, куда семя упало (минералы только
                // ходят по кругу, см. заголовок системы).
                for (const auto tile : w.area().cellAt(targetX, targetY).entities) {
                    if (auto* soilComponent = w.registry().try_get<SoilComponent>(tile)) {
                        soilComponent->minerals += seedling.minerals;
                        break;
                    }
                }
                return;
            }

            const auto entity = w.registry().create();
            w.registry().emplace<PositionComponent>(entity, PositionComponent{targetX, targetY});
            w.registry().emplace<PlantComponent>(entity, seedling);
            w.registry().emplace<PlantGenomeComponent>(entity, child);
            w.area().place(entity, targetX, targetY, /*impassable=*/false);
        });
    }

    // --- 3. Перегной: разложение и возврат минералов в почву ---
    // Ровно те крупицы, что растение при жизни вынуло из этой клетки,
    // возвращаются в неё же — по humusDecayRate за тик. Пока перегной
    // лежит, клетка постепенно становится плодороднее; когда возвращать
    // нечего, компонент снимается (02_CorePrinciples.md, п.3: нет
    // перегноя — нет и компонента).
    auto humusView = registry.view<HumusComponent, SoilComponent>();
    for (const auto entity : humusView) {
        auto& humus = humusView.get<HumusComponent>(entity);
        auto& soil = humusView.get<SoilComponent>(entity);

        humus.pending += humusDecayRate;
        while (humus.pending >= 1.0f && humus.minerals > 0) {
            humus.pending -= 1.0f;
            --humus.minerals;
            ++soil.minerals;
        }
        humus.pending = std::min(humus.pending, 1.0f);

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

} // namespace goblins

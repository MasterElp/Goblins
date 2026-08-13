#include "core/systems/HydrologySystem.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "core/components/HeightComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Расстояние до соседа по каждому из восьми направлений. Нужно там, где
// считается именно УКЛОН (падение уровня на единицу расстояния), а не
// просто разница высот: по диагонали до соседа дальше в sqrt(2) раз, и
// без этой поправки диагональные направления выигрывают конкуренцию за
// сток чаще, чем должны — вода расползается характерным квадратом
// вместо круга. Для BFS-дистанции до воды и для минералов (там всё
// решает не уклон, а сам факт соседства) поправка не нужна.
constexpr float kDiagonal = 1.41421356f;
constexpr float kDist8[8] = {kDiagonal, 1.0f, kDiagonal, 1.0f, 1.0f, kDiagonal, 1.0f, kDiagonal};

// Влажность: та же форма, что и в TerrainGenerator.cpp (waterBoost по
// расстоянию до воды, ослабление каменистостью), но здесь это не разовый
// расчёт, а цель, к которой влажность медленно (kMoistureAdaptRate за тик)
// движется — отсюда и постепенное высыхание вдали от воды (target -> 0), и
// постепенное увлажнение рядом с ней.
constexpr float kMoistureFalloff = 8.0f;
constexpr float kWaterMoistureBoost = 0.7f;
constexpr float kRockMoistureReduction = 0.3f;
constexpr float kMoistureAdaptRate = 0.01f;

// Утрамбованность: только размягчение, необратимо (без причины со стороны
// воды утрамбованность не меняется — 02_CorePrinciples.md, п.12). rockFloor —
// доля исходной утрамбованности, ниже которой каменистый участок не
// размягчается (камень остаётся твёрдым даже у самой воды).
constexpr float kCompactionRockFloor = 0.6f;
constexpr float kCompactionSoftenReach = 4.0f;
constexpr float kCompactionSoftenRate = 0.02f;

// Эрозия сопровождает перенос воды: источник теряет высоту, приёмник
// получает ровно столько же (сохранение "материала", без источников из
// ниоткуда) — масштабируется каменистостью (сопротивляется эрозии) и текущей
// мягкостью почвы (что мягче — то быстрее размывается), давая петлю обратной
// связи с размягчением из п.4.
constexpr float kErosionRate = 0.05f;

// Гистерезис появления/исчезания WaterComponent — без него тайл на границе
// порога мерцал бы туда-сюда каждый тик.
constexpr float kWaterAppearThreshold = 0.05f;
constexpr float kWaterDisappearThreshold = 0.001f;

// Минералы: выравнивание с единственным соседом за тик, а не раздача
// всем сразу. Клетка A ищет среди соседей, которые "притягивают" минералы
// (есть вода или влажность выше порога — они "вымываются" туда), самого
// бедного B; если A богаче B минимум на kMineralSlopeThreshold, к B уходит
// ровно половина разницы (целочисленно) — 8 и 6 становятся 7 и 7, 10 и 6
// — 8 и 8. Обязательно ОДИН сосед, не несколько: если считать перетоки к
// нескольким соседям независимо по одному и тому же снимку (как было
// раньше), клетка может отдать больше, чем у неё есть, и вместо
// предсказуемого сглаживания получаются на вид хаотичные "волны" — тот же
// приём единственного лучшего соседа, что и у течения воды выше. Порог
// влажности — не константа здесь, а свойство мира
// (WorldPropertiesComponent.mineralMoistureThreshold, см. 06_GameLoop.md,
// п.1a): выбирается один раз при генерации, System его только читает.
constexpr int kMineralSlopeThreshold = 2;

} // namespace

void HydrologySystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    // Свойства мира, выбранные один раз при генерации — System только
    // читает, никогда не пишет (06_GameLoop.md, п.1a).
    const auto& worldProperties = world.registry().get<const WorldPropertiesComponent>(world.worldEntity());
    const float mineralMoistureThreshold = worldProperties.mineralMoistureThreshold;
    const float waterEvaporationRate = worldProperties.waterEvaporationRate;
    const float waterSourceStrength = worldProperties.waterSourceStrength;
    const float waterFlowRate = worldProperties.waterFlowRate;
    const float waterSlopeBoost = worldProperties.waterSlopeBoost;

    // --- 1. Снимок текущего состояния ---
    // entt::null не подставляется вторым аргументом vector(count, value)
    // напрямую: у него шаблонный неограниченный operator T(), из-за чего
    // MSVC при разборе перегрузок конструктора vector пытается
    // сконвертировать его ещё и в Allocator и падает с ошибкой внутри
    // entt.hpp — поэтому конвертируем в entt::entity явно, заранее.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> entities(cellCount, kNullEntity);
    std::vector<float> moisture(cellCount, 0.0f);
    std::vector<float> rockiness(cellCount, 0.0f);
    std::vector<float> compaction(cellCount, 0.0f);
    std::vector<int> minerals(cellCount, 0);
    std::vector<float> terrainHeight(cellCount, 0.0f);
    std::vector<float> waterDepth(cellCount, 0.0f);
    std::vector<float> flowSpeed(cellCount, 0.0f);
    std::vector<bool> isWaterSource(cellCount, false);

    auto& registry = world.registry();
    auto view = registry.view<PositionComponent, SoilComponent, HeightComponent>();
    for (const auto entity : view) {
        const auto& position = view.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        const auto& soil = view.get<SoilComponent>(entity);
        const auto& heightComponent = view.get<HeightComponent>(entity);

        entities[i] = entity;
        moisture[i] = soil.moisture;
        rockiness[i] = soil.rockiness;
        compaction[i] = soil.compaction;
        minerals[i] = soil.minerals;
        terrainHeight[i] = heightComponent.height;

        if (const auto* water = registry.try_get<WaterComponent>(entity)) {
            waterDepth[i] = water->depth;
            flowSpeed[i] = water->flowSpeed;
        }
        isWaterSource[i] = registry.all_of<WaterSourceComponent>(entity);
    }

    // --- 2. Дистанция до воды: multi-source BFS (8-связность), заново
    // каждый тик — вода двигается, поэтому карта дистанций не статична. ---
    std::vector<int> distanceToWater(cellCount, -1);
    std::queue<int> bfs;
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (waterDepth[i] > 0.0f) {
            distanceToWater[i] = 0;
            bfs.push(static_cast<int>(i));
        }
    }
    while (!bfs.empty()) {
        const int idx = bfs.front();
        bfs.pop();
        const int x = idx % width;
        const int y = idx / width;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t ni = index(nx, ny);
            if (distanceToWater[ni] == -1) {
                distanceToWater[ni] = distanceToWater[static_cast<std::size_t>(idx)] + 1;
                bfs.push(static_cast<int>(ni));
            }
        }
    }

    // --- 3. Влажность и 4. утрамбованность: релаксация к цели ---
    std::vector<float> nextMoisture(moisture);
    std::vector<float> nextCompaction(compaction);
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (entities[i] == entt::null) {
            continue;
        }

        const float waterProximity =
            distanceToWater[i] >= 0 ? std::exp(-static_cast<float>(distanceToWater[i]) / kMoistureFalloff) : 0.0f;
        const float targetMoisture =
            std::clamp(waterProximity * kWaterMoistureBoost, 0.0f, 1.0f) * (1.0f - rockiness[i] * kRockMoistureReduction);
        nextMoisture[i] = moisture[i] + (targetMoisture - moisture[i]) * kMoistureAdaptRate;

        const float softenProximity =
            distanceToWater[i] >= 0 ? std::exp(-static_cast<float>(distanceToWater[i]) / kCompactionSoftenReach) : 0.0f;
        const float compactionFloor = rockiness[i] * kCompactionRockFloor;
        if (compaction[i] > compactionFloor) {
            nextCompaction[i] = compaction[i] - (compaction[i] - compactionFloor) * kCompactionSoftenRate * softenProximity;
        }
    }

    // --- 5. Течение + эрозия: снимок -> аккумуляторы, порядок обхода не
    // влияет на результат. ---
    std::vector<float> nextWaterDepth(waterDepth);
    std::vector<float> nextTerrainHeight(terrainHeight);
    std::vector<float> inflowFlowSpeed(cellCount, 0.0f);

    for (std::size_t i = 0; i < cellCount; ++i) {
        if (waterDepth[i] <= 0.0f) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;
        const float surface = terrainHeight[i] + waterDepth[i];

        // Направление стока — по самому КРУТОМУ спуску (падение уровня,
        // делённое на расстояние), а не просто по самому низкому соседу:
        // иначе диагональный сосед с тем же падением выигрывает наравне с
        // ортогональным, хотя он дальше, и вода расползается квадратом.
        int bestNeighbor = -1;
        float bestSlope = 0.0f;
        float bestSurface = surface;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t ni = index(nx, ny);
            const float neighborSurface = terrainHeight[ni] + waterDepth[ni];
            const float drop = surface - neighborSurface;
            if (drop <= 0.0f) {
                continue;
            }
            const float slope = drop / kDist8[dir];
            if (slope > bestSlope) {
                bestSlope = slope;
                bestSurface = neighborSurface;
                bestNeighbor = static_cast<int>(ni);
            }
        }

        if (bestNeighbor < 0) {
            continue;
        }
        const std::size_t j = static_cast<std::size_t>(bestNeighbor);
        const float diff = surface - bestSurface;
        // Чем круче склон, тем быстрее по нему течёт. С постоянной
        // скоростью, чтобы протолкнуть дальше постоянный приток, воде
        // приходилось копить у истока большой стоячий перепад (скорость
        // ограничена, значит нужен большой diff) — отсюда и "конус"
        // вокруг источника. Потолок 1.0 и множитель 0.5 ниже сохраняют
        // устойчивость: за тик уровни в паре в худшем случае ровно
        // сравняются, но не перехлестнутся, то есть колебаний
        // "туда-обратно" не возникает.
        const float rate = std::min(1.0f, waterFlowRate + bestSlope * waterSlopeBoost);
        const float amount = std::min(waterDepth[i], diff * 0.5f) * rate;
        if (amount <= 0.0f) {
            continue;
        }

        nextWaterDepth[i] -= amount;
        nextWaterDepth[j] += amount;
        if (flowSpeed[i] > inflowFlowSpeed[j]) {
            inflowFlowSpeed[j] = flowSpeed[i];
        }

        const float erosion = amount * kErosionRate * (1.0f - rockiness[i]) * (1.0f - compaction[i]);
        nextTerrainHeight[i] -= erosion;
        nextTerrainHeight[j] += erosion;
    }

    // --- 5b. Испарение + источники: независимые правки поверх nextWaterDepth
    // из течения выше — каждый тик всякая вода теряет waterEvaporationRate
    // глубины, а источники (истоки рек, "родники") получают приток
    // waterSourceStrength — свойства мира, обе читаются выше, не константы
    // (06_GameLoop.md, п.1a). waterSourceStrength — АБСОЛЮТНАЯ величина, не
    // множитель waterEvaporationRate: раньше была множителем, и при
    // маленьком испарении (по умолчанию) источник не мог угнаться за
    // собственным оттоком через течение выше — глубина проседала почти до
    // нуля независимо от того, насколько увеличивали "силу". Оба читают
    // снимок (waterDepth/isWaterSource), не друг друга и не результат
    // течения — порядок клеток не важен. ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (waterDepth[i] > 0.0f) {
            nextWaterDepth[i] -= waterEvaporationRate;
        }
        if (isWaterSource[i]) {
            nextWaterDepth[i] += waterSourceStrength;
        }
    }

    // --- 6. Минералы: снимок -> для каждой клетки найти ОДНОГО (самого
    // бедного из притягивающих) соседа -> аккумулятор — тот же приём, что
    // и течение воды выше, поэтому порядок обхода клеток не влияет на
    // результат. ---
    std::vector<int> nextMinerals(minerals);
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (entities[i] == entt::null || minerals[i] < kMineralSlopeThreshold) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;

        int bestNeighbor = -1;
        int bestMinerals = minerals[i];
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t j = index(nx, ny);
            const bool neighborAttractsMinerals = waterDepth[j] > 0.0f || moisture[j] > mineralMoistureThreshold;
            if (!neighborAttractsMinerals || minerals[j] >= bestMinerals) {
                continue;
            }
            bestMinerals = minerals[j];
            bestNeighbor = static_cast<int>(j);
        }

        if (bestNeighbor < 0 || minerals[i] - bestMinerals < kMineralSlopeThreshold) {
            continue;
        }
        const int amount = (minerals[i] - bestMinerals) / 2;
        nextMinerals[i] -= amount;
        nextMinerals[static_cast<std::size_t>(bestNeighbor)] += amount;
    }

    // --- 7/8. Запись обратно: значения существующих компонентов правятся
    // напрямую, появление/исчезание WaterComponent — через очередь команд
    // (05_Entity.md, п.5: структурные изменения не мгновенны). ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        const auto entity = entities[i];
        if (entity == entt::null) {
            continue;
        }

        auto& soil = registry.get<SoilComponent>(entity);
        soil.moisture = std::clamp(nextMoisture[i], 0.0f, 1.0f);
        soil.compaction = std::clamp(nextCompaction[i], 0.0f, 1.0f);
        soil.minerals = std::max(0, nextMinerals[i]);
        registry.get<HeightComponent>(entity).height = nextTerrainHeight[i];

        const float depth = std::max(0.0f, nextWaterDepth[i]);
        const bool hadWater = waterDepth[i] > 0.0f;

        if (depth > kWaterAppearThreshold) {
            if (hadWater) {
                registry.get<WaterComponent>(entity).depth = depth;
            } else {
                commands.enqueue([entity, depth, speed = inflowFlowSpeed[i]](World& w) {
                    if (!w.registry().valid(entity) || w.registry().all_of<WaterComponent>(entity)) {
                        return;
                    }
                    w.registry().emplace<WaterComponent>(entity, WaterComponent{depth, speed});
                });
            }
        } else if (depth <= kWaterDisappearThreshold) {
            if (hadWater) {
                commands.enqueue([entity](World& w) {
                    if (w.registry().valid(entity)) {
                        w.registry().remove<WaterComponent>(entity);
                    }
                });
            }
        } else if (hadWater) {
            // Гистерезисная полоса: компонент уже есть — просто обновляем
            // глубину, не убираем его, пока не пересечён нижний порог.
            registry.get<WaterComponent>(entity).depth = depth;
        }
    }
}

} // namespace goblins

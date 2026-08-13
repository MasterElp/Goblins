#include "core/systems/HydrologySystem.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "core/components/HeightComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

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

// Течение: доля "избытка" (половины разницы уровней поверхности) воды,
// перетекающая к самому низкому соседу за тик — маленькая, чтобы форма
// водоёмов менялась постепенно, а не скачками.
constexpr float kFlowRate = 0.03f;

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

} // namespace

void HydrologySystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

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
    std::vector<float> terrainHeight(cellCount, 0.0f);
    std::vector<float> waterDepth(cellCount, 0.0f);
    std::vector<float> flowSpeed(cellCount, 0.0f);

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
        terrainHeight[i] = heightComponent.height;

        if (const auto* water = registry.try_get<WaterComponent>(entity)) {
            waterDepth[i] = water->depth;
            flowSpeed[i] = water->flowSpeed;
        }
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

        int bestNeighbor = -1;
        float bestSurface = surface;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t ni = index(nx, ny);
            const float neighborSurface = terrainHeight[ni] + waterDepth[ni];
            if (neighborSurface < bestSurface) {
                bestSurface = neighborSurface;
                bestNeighbor = static_cast<int>(ni);
            }
        }

        if (bestNeighbor < 0) {
            continue;
        }
        const std::size_t j = static_cast<std::size_t>(bestNeighbor);
        const float diff = surface - bestSurface;
        const float amount = std::min(waterDepth[i], diff * 0.5f) * kFlowRate;
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

    // --- 6/7. Запись обратно: значения существующих компонентов правятся
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

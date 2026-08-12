#include "core/generation/TerrainGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <random>
#include <vector>

#include <fastnoiselite/FastNoiseLite.h>

#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

float normalize01(float noiseValue) {
    // FastNoiseLite возвращает примерно [-1, 1].
    return std::clamp((noiseValue + 1.0f) * 0.5f, 0.0f, 1.0f);
}

FastNoiseLite makeFbmNoise(int seed, float frequency, const TerrainParams& params) {
    FastNoiseLite noise(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(params.noiseOctaves);
    noise.SetFractalLacunarity(params.noiseLacunarity);
    noise.SetFractalGain(params.noiseGain);
    noise.SetFrequency(frequency);
    return noise;
}

struct HeightCell {
    float height;
    int index;
};

struct HeightCellGreater {
    bool operator()(const HeightCell& a, const HeightCell& b) const {
        return a.height > b.height;
    }
};

} // namespace

void generateTerrain(World& world, unsigned seed, const TerrainParams& params) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    // --- 1. Heightmap + почвенные параметры (fBm, разные seed/частоты) ---
    auto heightNoise = makeFbmNoise(static_cast<int>(seed), params.heightNoiseFrequency, params);
    auto rockNoise = makeFbmNoise(static_cast<int>(seed) + 1, params.rockNoiseFrequency, params);
    auto compactionNoise = makeFbmNoise(static_cast<int>(seed) + 2, params.compactionNoiseFrequency, params);
    auto moistureNoise = makeFbmNoise(static_cast<int>(seed) + 3, params.moistureNoiseFrequency, params);

    std::vector<float> elevation(cellCount);
    std::vector<float> rockiness(cellCount);
    std::vector<float> compaction(cellCount);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = index(x, y);
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);

            rockiness[i] = normalize01(rockNoise.GetNoise(fx, fy));
            compaction[i] = normalize01(compactionNoise.GetNoise(fx, fy));

            // Каменистые и утрамбованные участки физически выше — вода их
            // естественно огибает, поэтому там река не может
            // самостоятельно возникнуть (без ручных исключений).
            const float baseHeight = normalize01(heightNoise.GetNoise(fx, fy));
            elevation[i] = baseHeight + rockiness[i] * params.rockHeightBump + compaction[i] * params.compactionHeightBump;
        }
    }

    // --- 2. Priority-Flood (Barnes et al., 2014): заполнение впадин ---
    // filled[i] — минимальный уровень воды, при котором клетка i стекает
    // к краю карты. Если filled[i] > elevation[i], клетка лежит во
    // впадине — это пруд.
    std::vector<float> filled(cellCount, std::numeric_limits<float>::infinity());
    std::vector<bool> visited(cellCount, false);
    std::priority_queue<HeightCell, std::vector<HeightCell>, HeightCellGreater> queue;

    auto offer = [&](int x, int y, float level) {
        const std::size_t i = index(x, y);
        if (visited[i]) {
            return;
        }
        visited[i] = true;
        filled[i] = level;
        queue.push(HeightCell{level, static_cast<int>(i)});
    };

    for (int x = 0; x < width; ++x) {
        offer(x, 0, elevation[index(x, 0)]);
        offer(x, height - 1, elevation[index(x, height - 1)]);
    }
    for (int y = 0; y < height; ++y) {
        offer(0, y, elevation[index(0, y)]);
        offer(width - 1, y, elevation[index(width - 1, y)]);
    }

    while (!queue.empty()) {
        const HeightCell current = queue.top();
        queue.pop();
        const int cx = current.index % width;
        const int cy = current.index / width;

        for (int dir = 0; dir < 8; ++dir) {
            const int nx = cx + kDx8[dir];
            const int ny = cy + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            if (visited[index(nx, ny)]) {
                continue;
            }
            const float level = std::max(elevation[index(nx, ny)], current.height);
            offer(nx, ny, level);
        }
    }

    // --- 3. D8 flow direction + accumulation по заполненному рельефу ---
    // (после Priority-Flood у каждой клетки гарантированно есть путь
    // стока к краю карты).
    std::vector<int> flowTo(cellCount, -1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = index(x, y);
            float lowest = filled[i];
            int best = -1;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = x + kDx8[dir];
                const int ny = y + kDy8[dir];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    continue;
                }
                const std::size_t ni = index(nx, ny);
                if (filled[ni] < lowest) {
                    lowest = filled[ni];
                    best = static_cast<int>(ni);
                }
            }
            flowTo[i] = best; // -1, если сток уже достиг края
        }
    }

    // Обрабатываем клетки от высокой к низкой — топологически корректный
    // порядок для накопления стока по DAG направлений D8.
    std::vector<int> order(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return filled[a] > filled[b]; });

    std::vector<float> accumulation(cellCount, 1.0f);

    // Река "может начинаться за краем карты" — часть граничных клеток
    // получает случайный бонус к накоплению, как будто снаружи уже течёт
    // поток.
    std::mt19937 edgeRng(seed + 4);
    std::uniform_real_distribution<float> edgeBoost(0.0f, params.edgeInflowMax);
    for (std::size_t i = 0; i < cellCount; ++i) {
        const int x = static_cast<int>(i % width);
        const int y = static_cast<int>(i / width);
        const bool onEdge = (x == 0 || y == 0 || x == width - 1 || y == height - 1);
        if (onEdge) {
            accumulation[i] += edgeBoost(edgeRng);
        }
    }

    for (int idx : order) {
        const int target = flowTo[idx];
        if (target >= 0) {
            accumulation[static_cast<std::size_t>(target)] += accumulation[static_cast<std::size_t>(idx)];
        }
    }

    // --- 4. Порог реки + глубина воды (реки и пруды) ---
    float maxAccumulation = 0.0f;
    for (float a : accumulation) {
        maxAccumulation = std::max(maxAccumulation, a);
    }

    std::vector<float> waterDepth(cellCount, 0.0f);

    // Пруды: связные (8-связность, как и весь остальной D8-расчёт)
    // области впадин глубже params.minPondDepth; params.minPondSize /
    // params.maxPondSize ограничивают размер (0 у max — без ограничения).
    std::vector<bool> pondCandidate(cellCount, false);
    for (std::size_t i = 0; i < cellCount; ++i) {
        pondCandidate[i] = (filled[i] - elevation[i]) > params.minPondDepth;
    }

    std::vector<bool> pondVisited(cellCount, false);
    std::vector<int> componentBuffer;
    componentBuffer.reserve(64);
    for (std::size_t start = 0; start < cellCount; ++start) {
        if (!pondCandidate[start] || pondVisited[start]) {
            continue;
        }

        componentBuffer.clear();
        std::queue<int> componentBfs;
        pondVisited[start] = true;
        componentBfs.push(static_cast<int>(start));
        componentBuffer.push_back(static_cast<int>(start));

        while (!componentBfs.empty()) {
            const int idx = componentBfs.front();
            componentBfs.pop();
            const int x = idx % width;
            const int y = idx / width;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = x + kDx8[dir];
                const int ny = y + kDy8[dir];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    continue;
                }
                const std::size_t ni = index(nx, ny);
                if (pondCandidate[ni] && !pondVisited[ni]) {
                    pondVisited[ni] = true;
                    componentBfs.push(static_cast<int>(ni));
                    componentBuffer.push_back(static_cast<int>(ni));
                }
            }
        }

        const bool tooSmall = static_cast<int>(componentBuffer.size()) < params.minPondSize;
        const bool tooBig = params.maxPondSize > 0 && static_cast<int>(componentBuffer.size()) > params.maxPondSize;
        if (tooSmall || tooBig) {
            continue;
        }
        for (int idx : componentBuffer) {
            const std::size_t i = static_cast<std::size_t>(idx);
            const float pondDepth = filled[i] - elevation[i];
            waterDepth[i] = std::max(waterDepth[i], pondDepth * params.pondDepthScale);
        }
    }

    for (std::size_t i = 0; i < cellCount; ++i) {
        if (accumulation[i] >= params.riverThreshold) {
            const float t = std::clamp((accumulation[i] - params.riverThreshold) /
                                            std::max(1.0f, maxAccumulation - params.riverThreshold),
                                        0.0f, 1.0f);
            waterDepth[i] = std::max(waterDepth[i], params.riverDepthBase + t * params.riverDepthRange);
        }
    }

    // --- 5. Влажность: фоновый шум + затухание по расстоянию до воды ---
    // Multi-source BFS от всех водных клеток — стандартный distance
    // transform, даёт плавный градиент вместо ступенчатого.
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

    // --- Создание Entity: один терраформирующий Entity на тайл ---
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = index(x, y);

            const float baseMoisture = normalize01(moistureNoise.GetNoise(static_cast<float>(x), static_cast<float>(y)));
            float waterBoost = 0.0f;
            if (distanceToWater[i] >= 0) {
                waterBoost = std::exp(-static_cast<float>(distanceToWater[i]) / params.moistureFalloff);
            }
            // Каменистая почва хуже держит влагу.
            const float moisture = std::clamp(
                baseMoisture * (1.0f - rockiness[i] * params.rockMoistureReduction) +
                    waterBoost * params.waterMoistureBoost,
                0.0f, 1.0f);

            const auto entity = world.registry().create();
            world.registry().emplace<PositionComponent>(entity, PositionComponent{x, y});
            world.registry().emplace<SoilComponent>(entity, SoilComponent{moisture, rockiness[i], compaction[i]});
            if (waterDepth[i] > 0.0f) {
                world.registry().emplace<WaterComponent>(entity, WaterComponent{waterDepth[i]});
            }
            world.area().place(entity, x, y, /*impassable=*/false);
        }
    }
}

} // namespace goblins

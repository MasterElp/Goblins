#include "core/generation/TerrainGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <random>
#include <utility>
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

// --- Реки: явные русла (путь + ширина), см. TerrainGenerator.hpp. ---

constexpr float kPi = 3.14159265358979323846f;

// Не параметры генерации (params зашивать их незачем — они управляют
// внутренней механикой построения пути, а не видимым результатом,
// который целиком определяют riverWidth/riverSinuosity/riverDepth).
constexpr float kMeanderFrequency = 1.0f / 12.0f; // один "изгиб" примерно на 12 тайлов длины пути
constexpr float kMaxMeanderFraction = 0.18f;      // амплитуда меандра как доля от прямого расстояния исток-устье
constexpr float kRiverCarveMultiplier = 2.0f;     // запас карвинга над riverDepth (перекрывает rock/compaction bumps)
constexpr int kRiverAttemptMultiplier = 100;      // maxAttempts = riverCount * 100, как в BoulderScatter

enum RiverEdge : int { kEdgeNorth = 0, kEdgeSouth = 1, kEdgeWest = 2, kEdgeEast = 3 };

struct PathPoint {
    float x;
    float y;
};

struct RiverCell {
    int index;
    float falloff; // 1 в центре русла, 0 на границе ширины
};

struct RiverCandidate {
    std::vector<RiverCell> footprint; // max-объединение по всем руслам (strand) одной реки
};

float smoothstep01(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

float edgeLength(int edge, int width, int height) {
    return (edge == kEdgeNorth || edge == kEdgeSouth) ? static_cast<float>(width) : static_cast<float>(height);
}

PathPoint edgePoint(int edge, float coord, int width, int height) {
    switch (edge) {
        case kEdgeNorth:
            return PathPoint{coord, 0.0f};
        case kEdgeSouth:
            return PathPoint{coord, static_cast<float>(height - 1)};
        case kEdgeWest:
            return PathPoint{0.0f, coord};
        default: // kEdgeEast
            return PathPoint{static_cast<float>(width - 1), coord};
    }
}

// Раскладывает numPoints точек по стороне карты в отдельные интервалы
// (bucket = index/count), со случайной позицией внутри интервала — так
// два истока/устья одной реки никогда не совпадают и не липнут к углам.
float pickEdgeCoordinate(std::mt19937& rng, float edgeLen, int bucketIndex, int bucketCount) {
    const float margin = std::max(1.0f, edgeLen * 0.05f);
    const float usable = std::max(1.0f, edgeLen - 1.0f - 2.0f * margin);
    const float bucketSize = usable / static_cast<float>(bucketCount);
    std::uniform_real_distribution<float> within(0.0f, bucketSize);
    const float coord = margin + static_cast<float>(bucketIndex) * bucketSize + within(rng);
    return std::clamp(coord, 0.0f, edgeLen - 1.0f);
}

// Плавная меандрирующая линия от p0 к p1: смещение перпендикулярно
// прямой p0->p1 задаётся 2D-шумом (переиспользуем уже подключённый
// FastNoiseLite, без новых зависимостей), амплитуда — от sinuosity.
// sin(pi*t)-огибающая обнуляет смещение на обоих концах, поэтому путь
// всегда точно упирается в исток/устье на границе карты.
std::vector<PathPoint> buildMeanderPath(PathPoint p0, PathPoint p1, float sinuosity, int noiseSeed) {
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1e-3f) {
        return {p0, p1};
    }

    FastNoiseLite noise(noiseSeed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(kMeanderFrequency);

    const float dirX = dx / dist;
    const float dirY = dy / dist;
    const float perpX = -dirY;
    const float perpY = dirX;
    const int steps = std::max(8, static_cast<int>(std::round(dist)));
    const float amplitude = dist * kMaxMeanderFraction * sinuosity;

    std::vector<PathPoint> points;
    points.reserve(static_cast<std::size_t>(steps) + 1);
    for (int s = 0; s <= steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        const float envelope = std::sin(kPi * t);
        const float n = noise.GetNoise(t * dist, 0.0f);
        const float offset = amplitude * envelope * n;
        points.push_back(PathPoint{p0.x + dirX * t * dist + perpX * offset, p0.y + dirY * t * dist + perpY * offset});
    }
    return points;
}

// Соседние сэмплы пути могут отстоять больше чем на тайл (меандр может
// сместить их сильно) — идём по каждому отрезку с шагом ~1 тайл, чтобы
// центральная линия русла была непрерывной.
std::vector<std::pair<int, int>> rasterizeCenterline(const std::vector<PathPoint>& points, int width, int height) {
    std::vector<std::pair<int, int>> cells;

    auto pushCell = [&](int x, int y) {
        if (x < 0 || x >= width || y < 0 || y >= height) {
            return;
        }
        if (!cells.empty() && cells.back().first == x && cells.back().second == y) {
            return;
        }
        cells.emplace_back(x, y);
    };

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const PathPoint& a = points[i];
        const PathPoint& b = points[i + 1];
        const float segLen = std::hypot(b.x - a.x, b.y - a.y);
        const int subSteps = std::max(1, static_cast<int>(std::ceil(segLen)));
        for (int k = 0; k <= subSteps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(subSteps);
            pushCell(static_cast<int>(std::lround(a.x + (b.x - a.x) * t)),
                     static_cast<int>(std::lround(a.y + (b.y - a.y) * t)));
        }
    }
    if (cells.empty() && !points.empty()) {
        pushCell(static_cast<int>(std::lround(points.front().x)), static_cast<int>(std::lround(points.front().y)));
    }
    return cells;
}

// Штампует ширину русла вокруг центральной линии в falloffScratch,
// накапливая МАКСИМУМ (не сумму) на клетку — самопересекающийся меандр
// одного русла не должен вырезать/заливать клетку дважды. touched
// собирает список задетых индексов, чтобы вызывающий код мог быстро
// сбросить только их (карта scratch общая на все попытки размещения).
void stampFootprint(const std::vector<std::pair<int, int>>& centerline, float halfWidth, int width, int height,
                     std::vector<float>& falloffScratch, std::vector<int>& touched) {
    const int r = std::max(1, static_cast<int>(std::ceil(halfWidth)));
    for (const auto& [cx, cy] : centerline) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                const int nx = cx + dx;
                const int ny = cy + dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    continue;
                }
                const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > halfWidth) {
                    continue;
                }
                const float f = 1.0f - smoothstep01(d / halfWidth);
                if (f <= 0.0f) {
                    continue;
                }
                const std::size_t idx = static_cast<std::size_t>(ny) * width + nx;
                if (falloffScratch[idx] == 0.0f) {
                    touched.push_back(static_cast<int>(idx));
                }
                falloffScratch[idx] = std::max(falloffScratch[idx], f);
            }
        }
    }
}

// Одна река: 1-2 истока на случайной стороне карты, 1-3 устья на другой
// случайной стороне; независимые русла (без слияния/разветвления) от
// каждого истока к устью (по кругу, если счётчики не совпадают).
RiverCandidate generateRiverCandidate(std::mt19937& rng, int width, int height, float halfWidth, float sinuosity,
                                       std::vector<float>& falloffScratch, std::vector<int>& touched) {
    std::uniform_int_distribution<int> edgeDist(0, 3);
    std::uniform_int_distribution<int> destOffsetDist(1, 3);
    std::uniform_int_distribution<int> sourceCountDist(1, 2);
    std::uniform_int_distribution<int> mouthCountDist(1, 3);

    const int sourceEdge = edgeDist(rng);
    const int destEdge = (sourceEdge + destOffsetDist(rng)) % 4;
    const int numSources = sourceCountDist(rng);
    const int numMouths = mouthCountDist(rng);

    const float sourceEdgeLen = edgeLength(sourceEdge, width, height);
    const float destEdgeLen = edgeLength(destEdge, width, height);

    std::vector<PathPoint> sourcePoints;
    sourcePoints.reserve(static_cast<std::size_t>(numSources));
    for (int i = 0; i < numSources; ++i) {
        sourcePoints.push_back(
            edgePoint(sourceEdge, pickEdgeCoordinate(rng, sourceEdgeLen, i, numSources), width, height));
    }
    std::vector<PathPoint> mouthPoints;
    mouthPoints.reserve(static_cast<std::size_t>(numMouths));
    for (int i = 0; i < numMouths; ++i) {
        mouthPoints.push_back(edgePoint(destEdge, pickEdgeCoordinate(rng, destEdgeLen, i, numMouths), width, height));
    }

    touched.clear();
    const int strandCount = std::max(numSources, numMouths);
    for (int s = 0; s < strandCount; ++s) {
        const PathPoint& p0 = sourcePoints[static_cast<std::size_t>(s % numSources)];
        const PathPoint& p1 = mouthPoints[static_cast<std::size_t>(s % numMouths)];
        const int strandSeed = static_cast<int>(rng());
        const auto meander = buildMeanderPath(p0, p1, sinuosity, strandSeed);
        const auto centerline = rasterizeCenterline(meander, width, height);
        stampFootprint(centerline, halfWidth, width, height, falloffScratch, touched);
    }

    RiverCandidate candidate;
    candidate.footprint.reserve(touched.size());
    for (int idx : touched) {
        candidate.footprint.push_back(RiverCell{idx, falloffScratch[static_cast<std::size_t>(idx)]});
        falloffScratch[static_cast<std::size_t>(idx)] = 0.0f; // сброс для следующей попытки
    }
    return candidate;
}

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

    // --- 1b. Реки: пути + вырезание русла в elevation, ДО Priority-Flood ---
    // Карвинг делается раньше заливки впадин, чтобы пруды и реки
    // согласовывались сами по себе (пруд может естественно образоваться
    // поверх/вдоль вырезанного русла — река его "пересекает" без
    // отдельной логики совмещения).
    std::mt19937 riverRng(seed + 10);
    const float riverHalfWidth = std::max(0.5f, params.riverWidth * 0.5f);
    std::vector<RiverCandidate> acceptedRivers;
    {
        std::vector<float> falloffScratch(cellCount, 0.0f);
        std::vector<int> touched;
        std::vector<bool> riverOccupied(cellCount, false);
        const int maxAttempts = std::max(0, params.riverCount) * kRiverAttemptMultiplier;
        int placed = 0;
        int attempts = 0;
        while (placed < params.riverCount && attempts < maxAttempts) {
            ++attempts;
            RiverCandidate candidate = generateRiverCandidate(riverRng, width, height, riverHalfWidth,
                                                                params.riverSinuosity, falloffScratch, touched);
            bool collides = false;
            for (const auto& cell : candidate.footprint) {
                if (riverOccupied[static_cast<std::size_t>(cell.index)]) {
                    collides = true;
                    break;
                }
            }
            if (collides) {
                // Непересекающиеся реки (требование): при коллизии просто
                // пробуем другой случайный путь, как rejection sampling в
                // BoulderScatter. При исчерпании попыток река молча не
                // размещается — та же конвенция, что и у булыжников.
                continue;
            }
            for (const auto& cell : candidate.footprint) {
                riverOccupied[static_cast<std::size_t>(cell.index)] = true;
            }
            acceptedRivers.push_back(std::move(candidate));
            ++placed;
        }
    }

    const float riverCarveDepth = params.riverDepth * kRiverCarveMultiplier;
    for (const auto& river : acceptedRivers) {
        for (const auto& cell : river.footprint) {
            elevation[static_cast<std::size_t>(cell.index)] -= riverCarveDepth * cell.falloff;
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

    // --- 3. Глубина воды: пруды (ниже) и реки (после, блок 4) ---
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

    // --- 4. Реки: глубина воды + скорость потока по вырезанным руслам ---
    // После прудов — на пересечении реки с прудом глубина берётся как
    // max (как и для двух прудов выше), а скорость потока всё равно > 0
    // (вода течёт даже через разлив). У пруда flowSpeed остаётся 0.
    std::vector<float> flowSpeed(cellCount, 0.0f);
    for (const auto& river : acceptedRivers) {
        for (const auto& cell : river.footprint) {
            const std::size_t i = static_cast<std::size_t>(cell.index);
            waterDepth[i] = std::max(waterDepth[i], params.riverDepth * cell.falloff);
            if (cell.falloff > 0.0f) {
                flowSpeed[i] = std::max(flowSpeed[i], params.riverFlowSpeed);
            }
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
                world.registry().emplace<WaterComponent>(entity, WaterComponent{waterDepth[i], flowSpeed[i]});
            }
            world.area().place(entity, x, y, /*impassable=*/false);
        }
    }
}

} // namespace goblins

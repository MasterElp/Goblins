#include "core/generation/TerrainGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fastnoiselite/FastNoiseLite.h>

#include "core/components/HeightComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"

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
// внутренней механикой построения пути/шума, а не видимым результатом,
// который целиком определяют riverWidth/riverSinuosity/riverDepth/
// riverMaxFlowSpeed).
constexpr float kMeanderFrequency = 1.0f / 12.0f; // один "изгиб" примерно на 12 тайлов длины пути
constexpr float kMeanderPushScale = 1.4f;         // сила виляния от шума
constexpr float kSoilProbeDist = 2.0f;            // на сколько тайлов пробуем рельеф по бокам от курса
constexpr float kSoilPushScale = 10.0f;           // во что превращается разница высот в боковой толчок
constexpr float kSoilPushClamp = 1.5f;            // потолок бокового толчка от рельефа — не должен доминировать над шумом
// Жёсткий предел суммарного бокового отклонения за шаг (доли stepLen).
// Без него при резких перепадах рельефа (высокие rock/compaction bump +
// высокая частота шума) soilPush мог быть огромным, путь "телепортировался"
// на десятки тайлов за шаг — resulting samples.size() и стоимость
// stampFootprint взрывались, генерация зависала на потоке GameLoop.
constexpr float kMaxLateralPerStep = 2.5f;
constexpr float kMinSpeedFraction = 0.3f; // минимальная доля от riverMaxFlowSpeed у самой медленной реки
constexpr float kWidthNoiseFrequency = 1.0f / 10.0f;
constexpr float kWidthNoiseAmplitude = 0.5f;
constexpr float kWidthMinMul = 0.5f;
constexpr float kWidthMaxMul = 1.6f;
constexpr float kDepthNoiseFrequency = 1.0f / 8.0f;
constexpr float kDepthNoiseAmplitude = 0.35f;
constexpr float kDepthMinMul = 0.6f;
constexpr float kDepthMaxMul = 1.4f;
constexpr float kRiverCarveMultiplier = 2.0f; // запас карвинга над riverDepth (перекрывает rock/compaction bumps)
constexpr int kRiverAttemptMultiplier = 100;  // maxAttempts = riverCount * 100, как в BoulderScatter
constexpr float kMergeProbability = 0.5f;     // при столкновении с уже принятой рекой — шанс слиться, а не отклонить путь
constexpr float kMergeMarginFraction = 0.15f; // не сливаемся у самого истока целевой реки (первые 15% её длины)

// Подстраховки диагностики (GenerationStats), а не поведения "по умолчанию":
// при нормальных параметрах ни одна не должна срабатывать. Если сработала —
// riverTimedOut/предупреждение в консоли сразу укажут, где искать баг,
// вместо тихого зависания GameLoop.
constexpr double kRiverStageDeadlineMs = 2000.0;   // жёсткий потолок на всю стадию размещения рек
constexpr std::size_t kMaxRiverPathSamples = 4000; // потолок на длину одного пути (после kMaxLateralPerStep практически недостижим)

enum RiverEdge : int { kEdgeNorth = 0, kEdgeSouth = 1, kEdgeWest = 2, kEdgeEast = 3 };

struct PathPoint {
    float x;
    float y;
};

// Одна точка центральной линии русла: своя ширина/множитель глубины
// (оба — с шумом вдоль пути, чтобы русло не выглядело трубой постоянного
// сечения). halfWidth может быть увеличена позже, если в эту реку
// вливается другая (слияние — шире после точки слияния).
struct RiverPathSample {
    int x;
    int y;
    float halfWidth;
    float depthMul;
};

struct RiverCell {
    float falloff;  // 1 в центре русла, 0 на границе ширины (в этой точке)
    float depthMul; // множитель глубины ячейки, унаследованный от сэмпла с максимальным falloff
};

struct River {
    std::vector<RiverPathSample> centerline;      // порядок: исток -> устье или точка слияния в другую реку
    std::unordered_map<int, RiverCell> footprint; // index клетки -> данные; map, т.к. слияние дописывает существующие реки
    float flowSpeed = 0.0f;                       // собственная случайная скорость этой реки (<= riverMaxFlowSpeed)
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

// Случайная точка на стороне карты, с отступом от углов — исток/устье
// реки никогда не липнет к самому углу.
float pickEdgeCoordinate(std::mt19937& rng, float edgeLen) {
    const float margin = std::max(1.0f, edgeLen * 0.05f);
    const float usable = std::max(1.0f, edgeLen - 1.0f - 2.0f * margin);
    std::uniform_real_distribution<float> within(0.0f, usable);
    return std::clamp(margin + within(rng), 0.0f, edgeLen - 1.0f);
}

float sampleNearest(const std::vector<float>& field, int width, int height, float x, float y) {
    const int ix = std::clamp(static_cast<int>(std::lround(x)), 0, width - 1);
    const int iy = std::clamp(static_cast<int>(std::lround(y)), 0, height - 1);
    return field[static_cast<std::size_t>(iy) * width + ix];
}

// Путь от p0 к p1, шаг за шагом: на каждом шаге курс держим на цель
// (гарантированно дойдём), но подмешиваем боковое отклонение из двух
// источников — шум (собственно "меандр") и лёгкий толчок в сторону, где
// рельеф ниже ("немного учитывает почву и меняет направление", а не идёт
// напролом). Оба источника глушатся (а) sin(pi*t)-огибающей — не виляем
// у самых концов, путь точно приходит в p1 — и (б) wander = 1-speedFraction:
// чем быстрее река, тем меньше wander, тем прямее путь (в пределе
// wander=0 у самой быстрой реки — идеально прямая линия).
std::vector<PathPoint> buildMeanderPath(PathPoint p0, PathPoint p1, float sinuosity, float wander, int noiseSeed,
                                         const std::vector<float>& elevation, int width, int height) {
    const float totalDist = std::hypot(p1.x - p0.x, p1.y - p0.y);
    if (totalDist < 1e-3f) {
        return {p0, p1};
    }

    FastNoiseLite noise(noiseSeed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(kMeanderFrequency);

    const int steps = std::max(8, static_cast<int>(std::round(totalDist)));
    const float stepLen = totalDist / static_cast<float>(steps);

    std::vector<PathPoint> points;
    points.reserve(static_cast<std::size_t>(steps) + 2);
    points.push_back(p0);

    PathPoint pos = p0;
    float traveled = 0.0f;
    for (int s = 1; s < steps; ++s) {
        const float toTargetX = p1.x - pos.x;
        const float toTargetY = p1.y - pos.y;
        const float toTargetDist = std::hypot(toTargetX, toTargetY);
        if (toTargetDist < stepLen) {
            break; // остаток пути достроит финальная точка p1 ниже
        }
        const float fwdX = toTargetX / toTargetDist;
        const float fwdY = toTargetY / toTargetDist;
        const float perpX = -fwdY;
        const float perpY = fwdX;

        const float t = std::clamp(traveled / totalDist, 0.0f, 1.0f);
        const float envelope = std::sin(kPi * t);

        const float meanderPush = noise.GetNoise(traveled, 0.0f) * kMeanderPushScale;

        const float probeX = perpX * kSoilProbeDist;
        const float probeY = perpY * kSoilProbeDist;
        const float eLeft = sampleNearest(elevation, width, height, pos.x + probeX, pos.y + probeY);
        const float eRight = sampleNearest(elevation, width, height, pos.x - probeX, pos.y - probeY);
        const float soilPush = std::clamp((eRight - eLeft) * kSoilPushScale, -kSoilPushClamp, kSoilPushClamp);

        const float lateral =
            std::clamp((meanderPush + soilPush) * sinuosity * envelope * wander, -kMaxLateralPerStep, kMaxLateralPerStep);

        pos.x = std::clamp(pos.x + fwdX * stepLen + perpX * lateral * stepLen, 0.0f, static_cast<float>(width - 1));
        pos.y = std::clamp(pos.y + fwdY * stepLen + perpY * lateral * stepLen, 0.0f, static_cast<float>(height - 1));
        points.push_back(pos);
        traveled += stepLen;
    }
    points.push_back(p1);
    return points;
}

// Раскладывает путь на целочисленные клетки (~1 тайл на подшаг, чтобы не
// было разрывов) и на каждой клетке сэмплирует независимый шум ширины и
// глубины по пройденной длине — русло "дышит" по толщине и глубине
// вместо постоянного сечения.
std::vector<RiverPathSample> buildPathSamples(const std::vector<PathPoint>& points, int width, int height,
                                               float baseHalfWidth, int widthNoiseSeed, int depthNoiseSeed,
                                               bool& capped) {
    std::vector<RiverPathSample> samples;
    capped = false;

    FastNoiseLite widthNoise(widthNoiseSeed);
    widthNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    widthNoise.SetFrequency(kWidthNoiseFrequency);
    FastNoiseLite depthNoise(depthNoiseSeed);
    depthNoise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    depthNoise.SetFrequency(kDepthNoiseFrequency);

    auto pushSample = [&](int x, int y, float arcLen) {
        if (x < 0 || x >= width || y < 0 || y >= height) {
            return;
        }
        if (!samples.empty() && samples.back().x == x && samples.back().y == y) {
            return;
        }
        if (samples.size() >= kMaxRiverPathSamples) {
            capped = true;
            return;
        }
        const float wN = widthNoise.GetNoise(arcLen, 0.0f);
        const float dN = depthNoise.GetNoise(arcLen, 0.0f);
        const float halfWidth =
            std::max(0.5f, baseHalfWidth * std::clamp(1.0f + kWidthNoiseAmplitude * wN, kWidthMinMul, kWidthMaxMul));
        const float depthMul = std::clamp(1.0f + kDepthNoiseAmplitude * dN, kDepthMinMul, kDepthMaxMul);
        samples.push_back(RiverPathSample{x, y, halfWidth, depthMul});
    };

    float arcLen = 0.0f;
    for (std::size_t i = 0; i + 1 < points.size() && !capped; ++i) {
        const PathPoint& a = points[i];
        const PathPoint& b = points[i + 1];
        const float segLen = std::hypot(b.x - a.x, b.y - a.y);
        const int subSteps = std::max(1, static_cast<int>(std::ceil(segLen)));
        for (int k = 0; k <= subSteps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(subSteps);
            pushSample(static_cast<int>(std::lround(a.x + (b.x - a.x) * t)),
                       static_cast<int>(std::lround(a.y + (b.y - a.y) * t)), arcLen);
            arcLen += segLen / static_cast<float>(subSteps);
            if (capped) {
                break;
            }
        }
    }
    if (samples.empty() && !points.empty()) {
        pushSample(static_cast<int>(std::lround(points.front().x)), static_cast<int>(std::lround(points.front().y)),
                    0.0f);
    }
    return samples;
}

// Штампует ширину русла вокруг сэмплов [fromIdx, toIdxInclusive] в
// footprint, объединяя по МАКСИМУМУ falloff на клетку (не сумме) —
// самопересекающийся или расширенный (после слияния) путь не должен
// задвоить вклад на одну клетку.
void stampSegment(const std::vector<RiverPathSample>& samples, std::size_t fromIdx, std::size_t toIdxInclusive,
                   int width, int height, std::unordered_map<int, RiverCell>& footprint) {
    for (std::size_t si = fromIdx; si <= toIdxInclusive; ++si) {
        const RiverPathSample& sample = samples[si];
        const int r = std::max(1, static_cast<int>(std::ceil(sample.halfWidth)));
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                const int nx = sample.x + dx;
                const int ny = sample.y + dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    continue;
                }
                const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (d > sample.halfWidth) {
                    continue;
                }
                const float f = 1.0f - smoothstep01(d / sample.halfWidth);
                if (f <= 0.0f) {
                    continue;
                }
                const int idx = ny * width + nx;
                auto it = footprint.find(idx);
                if (it == footprint.end()) {
                    footprint.emplace(idx, RiverCell{f, sample.depthMul});
                } else if (f > it->second.falloff) {
                    it->second.falloff = f;
                    it->second.depthMul = sample.depthMul;
                }
            }
        }
    }
}

// Река, в которую влилась другая, становится шире НИЖЕ ПО ТЕЧЕНИЮ от
// точки слияния (mergePos — индекс в её собственной centerline).
// addedHalfWidth — полуширина вливающейся реки в точке стыка; ширины
// комбинируются как sqrt(a^2+b^2) — грубая, но достаточная имитация
// "больше воды => шире русло".
void widenDownstream(River& target, std::size_t mergePos, float addedHalfWidth, int width, int height,
                      std::vector<int>& riverOwner, int targetIndex) {
    for (std::size_t i = mergePos; i < target.centerline.size(); ++i) {
        RiverPathSample& sample = target.centerline[i];
        sample.halfWidth = std::sqrt(sample.halfWidth * sample.halfWidth + addedHalfWidth * addedHalfWidth);
    }
    stampSegment(target.centerline, mergePos, target.centerline.size() - 1, width, height, target.footprint);
    for (const auto& [idx, cell] : target.footprint) {
        if (riverOwner[static_cast<std::size_t>(idx)] == -1) {
            riverOwner[static_cast<std::size_t>(idx)] = targetIndex;
        }
    }
}

} // namespace

GenerationStats generateTerrain(World& world, unsigned seed, const TerrainParams& params) {
    using Clock = std::chrono::steady_clock;
    auto elapsedMs = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    const auto totalStart = Clock::now();
    GenerationStats stats;

    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    // --- 1. Heightmap + почвенные параметры (fBm, разные seed/частоты) ---
    const auto heightmapStart = Clock::now();
    auto heightNoise = makeFbmNoise(static_cast<int>(seed), params.heightNoiseFrequency, params);
    auto rockNoise = makeFbmNoise(static_cast<int>(seed) + 1, params.rockNoiseFrequency, params);
    auto compactionNoise = makeFbmNoise(static_cast<int>(seed) + 2, params.compactionNoiseFrequency, params);
    auto moistureNoise = makeFbmNoise(static_cast<int>(seed) + 3, params.moistureNoiseFrequency, params);
    auto mineralsNoise = makeFbmNoise(static_cast<int>(seed) + 4, params.mineralsNoiseFrequency, params);

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
    stats.heightmapMs = elapsedMs(heightmapStart);

    // --- 1b. Реки: пути + вырезание русла в elevation, ДО Priority-Flood ---
    // Карвинг делается раньше заливки впадин, чтобы пруды и реки
    // согласовывались сами по себе (пруд может естественно образоваться
    // поверх/вдоль вырезанного русла — река его "пересекает" без
    // отдельной логики совмещения).
    //
    // У каждой реки один случайный исток и один случайный конец (на
    // случайных сторонах карты) — путь между ними идёт напролом к цели,
    // но с боковым отклонением от шума и лёгкого притяжения к более
    // низкому рельефу, оба подавляются собственной случайной скоростью
    // реки (чем быстрее — тем прямее). Если по пути река утыкается в уже
    // принятую реку, она либо сливается с ней (обрывается в этой точке,
    // а целевая река становится шире ниже по течению), либо (с
    // дополняющей вероятностью) путь целиком отклоняется и пробуется
    // заново — так русла разных рек не наезжают друг на друга нигде,
    // кроме явных точек слияния.
    const auto riverStageStart = Clock::now();
    std::mt19937 riverRng(seed + 10);
    const float riverBaseHalfWidth = std::max(0.5f, params.riverWidth * 0.5f);
    std::vector<River> acceptedRivers;
    std::vector<int> riverOwner(cellCount, -1);
    stats.riversRequested = std::max(0, params.riverCount);
    {
        std::uniform_int_distribution<int> edgeDist(0, 3);
        std::uniform_int_distribution<int> destOffsetDist(1, 3);
        std::uniform_real_distribution<float> speedFractionDist(kMinSpeedFraction, 1.0f);
        std::uniform_real_distribution<float> mergeRoll(0.0f, 1.0f);

        const int maxAttempts = std::max(0, params.riverCount) * kRiverAttemptMultiplier;
        int placed = 0;
        int attempts = 0;
        while (placed < params.riverCount && attempts < maxAttempts) {
            // Защитный потолок по времени — см. GenerationStats::riverTimedOut.
            // При нормальных параметрах никогда не должен сработать (сама
            // стадия по построению ограничена maxAttempts дешёвых
            // итераций); это подстраховка от ещё не найденного крайнего
            // случая, чтобы поток GameLoop не мог зависнуть насмерть.
            if (elapsedMs(riverStageStart) > kRiverStageDeadlineMs) {
                stats.riverTimedOut = true;
                break;
            }
            ++attempts;

            const int sourceEdge = edgeDist(riverRng);
            const int destEdge = (sourceEdge + destOffsetDist(riverRng)) % 4;
            const PathPoint p0 = edgePoint(sourceEdge, pickEdgeCoordinate(riverRng, edgeLength(sourceEdge, width, height)),
                                            width, height);
            const PathPoint p1 = edgePoint(destEdge, pickEdgeCoordinate(riverRng, edgeLength(destEdge, width, height)),
                                            width, height);

            const float speedFraction = speedFractionDist(riverRng);
            const float wander = 1.0f - speedFraction;
            const int pathSeed = static_cast<int>(riverRng());
            const int widthSeed = static_cast<int>(riverRng());
            const int depthSeed = static_cast<int>(riverRng());

            const auto waypoints =
                buildMeanderPath(p0, p1, params.riverSinuosity, wander, pathSeed, elevation, width, height);
            bool pathCapped = false;
            auto samples =
                buildPathSamples(waypoints, width, height, riverBaseHalfWidth, widthSeed, depthSeed, pathCapped);
            if (pathCapped) {
                ++stats.riverPathsCapped;
            }
            if (samples.size() < 2) {
                continue;
            }

            // Ищем первое столкновение с уже принятой рекой вдоль
            // центральной линии — до этой точки путь гарантированно
            // "свой", после — либо слияние, либо отказ от всего пути.
            int collideAt = -1;
            int collideOwner = -1;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const std::size_t idx = index(samples[i].x, samples[i].y);
                if (riverOwner[idx] != -1) {
                    collideAt = static_cast<int>(i);
                    collideOwner = riverOwner[idx];
                    break;
                }
            }

            bool merged = false;
            std::size_t mergePos = 0;
            if (collideAt >= 0) {
                const River& targetRiver = acceptedRivers[static_cast<std::size_t>(collideOwner)];
                mergePos = targetRiver.centerline.size();
                for (std::size_t i = 0; i < targetRiver.centerline.size(); ++i) {
                    if (targetRiver.centerline[i].x == samples[static_cast<std::size_t>(collideAt)].x &&
                        targetRiver.centerline[i].y == samples[static_cast<std::size_t>(collideAt)].y) {
                        mergePos = i;
                        break;
                    }
                }
                const bool tooCloseToTargetSource =
                    mergePos <
                    static_cast<std::size_t>(kMergeMarginFraction * static_cast<float>(targetRiver.centerline.size()));
                const bool canMerge = mergePos < targetRiver.centerline.size() && !tooCloseToTargetSource;
                if (canMerge && mergeRoll(riverRng) < kMergeProbability) {
                    samples.resize(static_cast<std::size_t>(collideAt) + 1);
                    merged = true;
                } else {
                    continue; // отклоняем весь путь, пробуем другой в следующей попытке
                }
            }
            if (merged && samples.size() < 2) {
                continue; // слияние сразу у собственного истока — вырожденный случай, пробуем другой путь
            }

            River river;
            river.centerline = std::move(samples);
            river.flowSpeed = speedFraction * params.riverMaxFlowSpeed;
            stampSegment(river.centerline, 0, river.centerline.size() - 1, width, height, river.footprint);
            for (const auto& [idx, cell] : river.footprint) {
                riverOwner[static_cast<std::size_t>(idx)] = placed;
            }

            if (merged) {
                const float addedHalfWidth = river.centerline.back().halfWidth;
                widenDownstream(acceptedRivers[static_cast<std::size_t>(collideOwner)], mergePos, addedHalfWidth, width,
                                 height, riverOwner, collideOwner);
            }

            acceptedRivers.push_back(std::move(river));
            ++placed;
        }
        stats.riversPlaced = placed;
        stats.riverAttemptsUsed = attempts;
        stats.riverAttemptsMax = maxAttempts;
    }

    const float riverCarveDepth = params.riverDepth * kRiverCarveMultiplier;
    for (const auto& river : acceptedRivers) {
        for (const auto& [idx, cell] : river.footprint) {
            elevation[static_cast<std::size_t>(idx)] -= riverCarveDepth * cell.falloff;
        }
    }
    stats.riverMs = elapsedMs(riverStageStart); // путь+карвинг вместе — единая "стадия рек" для диагностики

    // --- 2. Priority-Flood (Barnes et al., 2014): заполнение впадин ---
    // filled[i] — минимальный уровень воды, при котором клетка i стекает
    // к краю карты. Если filled[i] > elevation[i], клетка лежит во
    // впадине — это пруд.
    const auto floodFillStart = Clock::now();
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

    stats.floodFillMs = elapsedMs(floodFillStart);

    // --- 3. Глубина воды: пруды (ниже) и реки (после, блок 4) ---
    const auto pondStart = Clock::now();
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
        ++stats.pondComponentsPlaced;
        for (int idx : componentBuffer) {
            const std::size_t i = static_cast<std::size_t>(idx);
            const float pondDepth = filled[i] - elevation[i];
            waterDepth[i] = std::max(waterDepth[i], pondDepth * params.pondDepthScale);
        }
    }
    stats.pondMs = elapsedMs(pondStart);

    // --- 4. Реки: глубина воды + скорость потока по вырезанным руслам ---
    // После прудов — на пересечении реки с прудом глубина берётся как
    // max (как и для двух прудов выше), а скорость потока всё равно > 0
    // (вода течёт даже через разлив). У пруда flowSpeed остаётся 0. depthMul
    // — шум глубины вдоль русла (buildPathSamples); flowSpeed — своя
    // случайная скорость этой реки (<= riverMaxFlowSpeed), а не общий
    // параметр на все реки.
    std::vector<float> flowSpeed(cellCount, 0.0f);
    for (const auto& river : acceptedRivers) {
        for (const auto& [idx, cell] : river.footprint) {
            const std::size_t i = static_cast<std::size_t>(idx);
            waterDepth[i] = std::max(waterDepth[i], params.riverDepth * cell.falloff * cell.depthMul);
            if (cell.falloff > 0.0f) {
                flowSpeed[i] = std::max(flowSpeed[i], river.flowSpeed);
            }
        }
    }

    // --- 5. Влажность: фоновый шум + затухание по расстоянию до воды ---
    // Multi-source BFS от всех водных клеток — стандартный distance
    // transform, даёт плавный градиент вместо ступенчатого.
    const auto moistureStart = Clock::now();
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

    stats.moistureMs = elapsedMs(moistureStart);

    // --- Создание Entity: один терраформирующий Entity на тайл ---
    const auto entityStart = Clock::now();
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

            // Минералы: на обычной почве — шум, в среднем дающий
            // params.mineralsAverage (noise01 в среднем ~0.5, поэтому
            // *2*mineralsAverage сходится к среднему mineralsAverage); на
            // клетках реки — не шум, а всегда фиксированное riverMinerals
            // (river/dwater flowSpeed[i] > 0 однозначно отличает реку от
            // пруда, где flowSpeed == 0 — см. WaterComponent).
            const float mineralsNoise01 = normalize01(mineralsNoise.GetNoise(static_cast<float>(x), static_cast<float>(y)));
            const int generatedMinerals =
                std::max(0, static_cast<int>(std::lround(mineralsNoise01 * params.mineralsAverage * 2.0f)));
            const int minerals = flowSpeed[i] > 0.0f ? params.riverMinerals : generatedMinerals;

            const auto entity = world.registry().create();
            world.registry().emplace<PositionComponent>(entity, PositionComponent{x, y});
            world.registry().emplace<SoilComponent>(entity, SoilComponent{moisture, rockiness[i], compaction[i], minerals});
            world.registry().emplace<HeightComponent>(entity, HeightComponent{elevation[i]});
            if (waterDepth[i] > 0.0f) {
                world.registry().emplace<WaterComponent>(entity, WaterComponent{waterDepth[i], flowSpeed[i]});
            }
            world.area().place(entity, x, y, /*impassable=*/false);
        }
    }
    stats.entityMs = elapsedMs(entityStart);

    // --- Свойства мира: выбираются один раз здесь, дальше System-ы (в
    // частности HydrologySystem) их только читают (06_GameLoop.md,
    // п.1a). WorldPropertiesComponent на World Entity уже существует
    // (создан в World::reset/конструкторе) — значения по умолчанию
    // просто перезаписываются выбором этой генерации.
    world.registry().get<WorldPropertiesComponent>(world.worldEntity()).mineralMoistureThreshold =
        params.mineralMoistureThreshold;

    stats.totalMs = elapsedMs(totalStart);
    return stats;
}

} // namespace goblins

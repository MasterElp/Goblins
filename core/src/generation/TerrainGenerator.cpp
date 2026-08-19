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

#include "core/Moisture.hpp"
#include "core/components/HeightComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/Diagnostics.hpp"
#include "core/Scale.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Канонические параметры формы fBm. Не в TerrainParams: их подстройка
// меняет картинку примерно тем же способом, что частота и число октав,
// только гораздо менее предсказуемо — две ручки на один эффект.
constexpr float kNoiseLacunarity = 2.0f;
constexpr float kNoiseGain = 0.5f;

// Частоты остальных слоёв — кратные от единственной настраиваемой
// (TerrainParams::featureSize). Слоям нужна не независимая настройка, а
// лишь разный масштаб узора, чтобы каменистость не повторяла рельеф один
// в один. Значения подобраны так, чтобы при featureSize = 50
// получались те же 0.05 и 0.06, что были раздельными параметрами. Третьего
// слоя (утрамбованность, 0.04) больше нет — вместе с самой утрамбованностью.
constexpr float kRockFrequencyRatio = 2.5f;
constexpr float kMineralsFrequencyRatio = 3.0f;

// Форма склона: во сколько раз "нажать" шум высоты перед растяжением до
// params.mountainHeight. Единица дала бы равномерный рельеф, где половина
// карты выше середины диапазона, — это не горы, а рябь. Со степенью выше
// единицы низины занимают бо́льшую часть карты, а вершины редки и высоки.
constexpr float kMountainSharpness = 2.5f;

// Какая доля самых высоких клеток карты считается "горами" — оттуда
// выбираются истоки рек. Доля, а не отметка высоты: "река начинается в
// горах" — форма закона, и она не должна зависеть от того, каким вышел
// рельеф конкретного мира.
constexpr float kRiverHeadTopFraction = 0.02f;

// ...но самые вершины из этого набора выбрасываются. На пике рельеф падает
// во все стороны сразу, поэтому и вода с него растекается во все стороны:
// исток на макушке горы даёт не реку, а расползающееся пятно. Чуть ниже
// вершины склон уже имеет одно направление, и река течёт в одну сторону —
// как и положено реке.
constexpr float kRiverHeadPeakSkipFraction = 0.005f;

float normalize01(float noiseValue) {
    // FastNoiseLite возвращает примерно [-1, 1].
    return std::clamp((noiseValue + 1.0f) * 0.5f, 0.0f, 1.0f);
}

FastNoiseLite makeFbmNoise(int seed, float frequency, const TerrainParams& params) {
    FastNoiseLite noise(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(params.noiseOctaves);
    noise.SetFractalLacunarity(kNoiseLacunarity);
    noise.SetFractalGain(kNoiseGain);
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
// который целиком определяют riverWidth/riverSinuosity/riverDepth).
constexpr float kMeanderFrequency = 1.0f / 12.0f; // один "изгиб" примерно на 12 тайлов длины пути
constexpr float kMeanderPushScale = 1.4f;         // сила виляния от шума
constexpr float kSoilProbeDist = 2.0f;            // на сколько тайлов пробуем рельеф по бокам от курса
constexpr float kSoilPushScale = 10.0f;           // во что превращается разница высот в боковой толчок
constexpr float kSoilPushClamp = 1.5f;            // потолок бокового толчка от рельефа — не должен доминировать над шумом
// Жёсткий предел суммарного бокового отклонения за шаг (доли stepLen).
// Без него при резких перепадах рельефа (высокая mountainHardness +
// высокая частота шума) soilPush мог быть огромным, путь "телепортировался"
// на десятки тайлов за шаг — resulting samples.size() и стоимость
// stampFootprint взрывались, генерация зависала на потоке GameLoop.
constexpr float kMaxLateralPerStep = 2.5f;
// Насколько сильно путь реки виляет: у каждой реки своя случайная доля из
// [0, kMaxWander]. Ноль — идеально прямая линия от истока к устью, чтобы
// реки одной карты не были похожи одна на другую.
constexpr float kMaxWander = 0.7f;
constexpr float kWidthNoiseFrequency = 1.0f / 10.0f;
constexpr float kWidthNoiseAmplitude = 0.5f;
constexpr float kWidthMinMul = 0.5f;
constexpr float kWidthMaxMul = 1.6f;
constexpr float kDepthNoiseFrequency = 1.0f / 8.0f;
constexpr float kDepthNoiseAmplitude = 0.35f;
constexpr float kDepthMinMul = 0.6f;
constexpr float kDepthMaxMul = 1.4f;
constexpr int kRiverAttemptMultiplier = 100;  // maxAttempts = riverCount * 100, как в BoulderScatter
constexpr float kMergeProbability = 0.5f;     // при столкновении с уже принятой рекой — шанс слиться, а не отклонить путь
constexpr float kMergeMarginFraction = 0.15f; // не сливаемся у самого истока целевой реки (первые 15% её длины)

// Минимальное падение уровня воды на один сэмпл пути. Уклон русла считается
// геометрически — от истока (вершина горы) ровно до устья (край мира,
// kVoidHeight), поделённое на длину пути: получается плавный ровный спуск на
// всю длину реки, а не заданная извне крутизна, которая на длинном пути
// уводит русло далеко ниже края карты, а на коротком не успевает спуститься.
//
// Эта константа — только нижняя страховка на вырожденный случай (исток
// оказался почти на уровне устья): без неё профиль перестал бы строго
// убывать, и вода встала бы в локальных ямах вместо того, чтобы течь.
constexpr float kRiverBedSlope = 2.0f;


// Берег: насколько кромка воды в русле идёт НИЖЕ окрестной земли. Русло
// вдавливается в рельеф на эту величину — не земля вокруг поднимается, а
// именно река врезается глубже.
//
// Без запаса поверхность реки вставала вровень с землёй (профиль начинался
// прямо с elevation истока), то есть русло было налито до краёв и берегов
// не имело вовсе. Любой соседний тайл, где земля хоть немного ниже, тут же
// становился кандидатом на сток: вода уходила вбок с первого же тика,
// вместо того чтобы идти по руслу. У истока росло мокрое пятно, а русло
// ниже сохло — при том, что само оно было построено правильно.
//
// Поднимать берег отдельным проходом (прежний kRiverBankHeight) для этого
// не нужно и вредно: подъём земли вокруг каждой реки лепил на карте валы,
// которых рельеф не предполагал. Тот же результат — "кромка ниже берега" —
// получается вдавливанием самого русла, и он честнее: реку прорезает вода,
// а не насыпь появляется сама.
//
// Не параметр: это условие того, что русло вообще является руслом, а не
// полосой разлива. Величина — заметно больше типичного перепада уровня
// между соседними клетками потока, иначе запаса не хватает: при единице
// река местами всё равно переливалась через край там, где рельеф рядом
// случайно проседал.
constexpr float kRiverFreeboard = 2000.0f;

// Минимальная глубина впадины, чтобы считаться прудом. Порог "это вообще
// впадина, а не численный шум Priority-Flood", а не настройка вида карты
// — размер пруда определяет рельеф, а не фильтр по числу тайлов (раньше
// таких фильтров было два, min/max, и оба стояли на "пропускать всё").
constexpr float kMinPondDepth = 10.0f;

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
    int sampleIndex; // тот же сэмпл: по нему берётся уровень поверхности воды в этой точке русла
};

struct River {
    std::vector<RiverPathSample> centerline;      // порядок: исток -> устье или точка слияния в другую реку
    std::unordered_map<int, RiverCell> footprint; // index клетки -> данные; map, т.к. слияние дописывает существующие реки
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
                    footprint.emplace(idx, RiverCell{f, sample.depthMul, static_cast<int>(si)});
                } else if (f > it->second.falloff) {
                    it->second.falloff = f;
                    it->second.depthMul = sample.depthMul;
                    it->second.sampleIndex = static_cast<int>(si);
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
    // Размер узора — в тайлах, шуму нужна обратная величина. Перевод один
    // раз здесь, на входе в генерацию: дальше внутри неё всё дробное, как
    // и положено способу посчитать (core/Scale.hpp).
    const float noiseFrequency = 1.0f / static_cast<float>(std::max(1, params.featureSize));
    auto heightNoise = makeFbmNoise(static_cast<int>(seed), noiseFrequency, params);
    auto rockNoise = makeFbmNoise(static_cast<int>(seed) + 1, noiseFrequency * kRockFrequencyRatio, params);
    auto mineralsNoise = makeFbmNoise(static_cast<int>(seed) + 4, noiseFrequency * kMineralsFrequencyRatio, params);

    std::vector<float> elevation(cellCount);
    std::vector<float> rockiness(cellCount);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = index(x, y);
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);

            rockiness[i] = normalize01(rockNoise.GetNoise(fx, fy));

            // Высота — чистый рельеф, без вклада почвы. Связь между
            // твёрдостью и высотой идёт в обратную сторону (см. ниже):
            // это гора делает почву каменистой, а не каменистость
            // поднимает гору.
            //
            // Шум возводится в степень (kMountainSharpness) и растягивается
            // до params.mountainHeight: линейно от шума получались не горы,
            // а пологая рябь — половина карты выше середины диапазона.
            // С нажимом бо́льшая часть карты остаётся низиной, а вершины
            // редки и по-настоящему высоки, поэтому и спуск от истока к
            // краю мира становится настоящим спуском.
            const float heightNoise01 = normalize01(heightNoise.GetNoise(fx, fy));
            const float relief = std::pow(heightNoise01, kMountainSharpness);
            elevation[i] = relief * static_cast<float>(params.mountainHeight);

            // Горы и их подножия сложены камнем: чем выше рельеф, тем
            // сильнее собственный шум каменистости подтягивается к самой
            // высоте. На mountainHardness = 0 остаётся прежний независимый
            // шум, на 1 — камень почти повторяет рельеф. Подмешивается
            // именно relief (доля от вершины, 0..1), а не сама высота:
            // каменистость нормализована, и абсолютная высота просто
            // упёрла бы её в единицу по всей карте.
            const float lift = static_cast<float>(std::clamp(params.mountainHardness, 0, kFull)) / kFull;
            rockiness[i] = rockiness[i] * (1.0f - lift) + relief * lift;
        }
    }
    stats.heightmapMs = elapsedMs(heightmapStart);

    // --- 1a. Вершины: клетки из верхней kRiverHeadTopFraction карты по
    // высоте. Оттуда берёт начало каждая река — исток в горах, устье на
    // краю мира (docs/01_Cosmology.md). Раньше река шла от случайной точки
    // на одном краю карты к случайной точке на другом, то есть текла
    // ниоткуда в никуда и вверх по рельефу ничуть не реже, чем вниз.
    //
    // Доля, а не отметка высоты: "исток в горах" — форма закона, и она не
    // должна ломаться от того, что у конкретного мира рельеф вышел ниже
    // или выше обычного. С фиксированной отметкой миры делились на "сотни
    // вершин" и "ни одной", и первое заливало карту водой.
    std::vector<int> riverHeadCells(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        riverHeadCells[i] = static_cast<int>(i);
    }
    {
        const auto higher = [&](int a, int b) {
            return elevation[static_cast<std::size_t>(a)] > elevation[static_cast<std::size_t>(b)];
        };
        const std::size_t keep = std::max<std::size_t>(
            1, static_cast<std::size_t>(static_cast<float>(cellCount) * kRiverHeadTopFraction));
        std::nth_element(riverHeadCells.begin(), riverHeadCells.begin() + static_cast<std::ptrdiff_t>(keep),
                          riverHeadCells.end(), higher);
        riverHeadCells.resize(keep);

        // И выбрасываем самые макушки: на пике склон падает во все стороны
        // сразу, и вода с него растекается пятном вместо реки.
        const std::size_t skip = std::min(
            keep - 1, static_cast<std::size_t>(static_cast<float>(cellCount) * kRiverHeadPeakSkipFraction));
        if (skip > 0) {
            std::nth_element(riverHeadCells.begin(), riverHeadCells.begin() + static_cast<std::ptrdiff_t>(skip),
                              riverHeadCells.end(), higher);
            riverHeadCells.erase(riverHeadCells.begin(), riverHeadCells.begin() + static_cast<std::ptrdiff_t>(skip));
        }
    }

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
    // Ширина приходит в десятых долях тайла (см. TerrainParams::riverWidth).
    const float riverBaseHalfWidth = std::max(0.5f, static_cast<float>(params.riverWidth) * 0.05f);
    std::vector<River> acceptedRivers;
    std::vector<int> riverOwner(cellCount, -1);
    stats.riversRequested = std::max(0, params.riverCount);
    {
        std::uniform_int_distribution<int> edgeDist(0, 3);
        std::uniform_int_distribution<int> headDist(0, static_cast<int>(riverHeadCells.size()) - 1);
        std::uniform_real_distribution<float> wanderDist(0.0f, kMaxWander);
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

            // Исток — случайная точка среди вершин, устье — случайная
            // точка на краю мира: река всегда течёт с горы к обрыву, а не
            // поперёк карты.
            const int headCell = riverHeadCells[static_cast<std::size_t>(headDist(riverRng))];
            const PathPoint p0{static_cast<float>(headCell % width), static_cast<float>(headCell / width)};
            const int destEdge = edgeDist(riverRng);
            const PathPoint p1 = edgePoint(destEdge, pickEdgeCoordinate(riverRng, edgeLength(destEdge, width, height)),
                                            width, height);

            const float wander = wanderDist(riverRng);
            const int pathSeed = static_cast<int>(riverRng());
            const int widthSeed = static_cast<int>(riverRng());
            const int depthSeed = static_cast<int>(riverRng());

            const auto waypoints =
                buildMeanderPath(p0, p1, static_cast<float>(params.riverSinuosity) / kFull, wander, pathSeed,
                                  elevation, width, height);
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

    // Профиль реки: сначала УРОВЕНЬ ПОВЕРХНОСТИ воды вдоль центральной
    // линии, потом дно как "поверхность минус глубина" — тот же приём и по
    // той же причине, что и у прудов ниже (elevation = filled - waterDepth).
    //
    // Раньше было наоборот: дно выравнивалось ПЛОСКО по всей ширине русла,
    // а глубина воды бралась куполом (riverDepth * falloff, где falloff = 1
    // в центре и 0 у берега). Плоское дно плюс купол глубины дают КУПОЛ
    // ПОВЕРХНОСТИ: посреди русла вода стояла выше, чем у берегов, и
    // HydrologySystem на первом же тике расплёскивал её вбок — отсюда рябь
    // поперёк реки и "глубокий тайл рядом с мелким". Поперёк русла ровной
    // обязана быть ПОВЕРХНОСТЬ, а неровным — дно.
    //
    // Уровень поверхности монотонно убывает от истока к устью: минимум
    // kRiverBedSlope за сэмпл. Без этого русло — просто траншея в шумном
    // рельефе, она наследует все его подъёмы и спуски, вода стекает в
    // ближайший локальный минимум внутри траншеи и стоит там, потому что
    // "вниз по руслу" физически не существует.
    //
    // min() с рельефом — поверхность реки не может оказаться выше земли, а
    // где земля падает круче, там падает и река (это нижняя граница
    // уклона, а не жёсткая линейка). min() с riverSurface — "садимся" на
    // уже размещённую реку: порядок обхода = порядок размещения, а
    // втекающая река всегда размещалась позже целевой, поэтому её устье
    // приходит ровно на поверхность целевой, без ступеньки в стыке.
    //
    // Отдельного "запаса карвинга" (прежний kRiverCarveMultiplier) больше
    // не нужно: elevation = min(elevation, surface - depth) срезает любой
    // бугор рельефа до дна русла по построению, поднятиям от каменистости
    // и утрамбованности просто негде проступить.
    std::vector<float> riverSurface(cellCount, std::numeric_limits<float>::infinity());
    std::vector<float> surfaceProfile;
    for (const auto& river : acceptedRivers) {
        if (river.centerline.empty()) {
            continue;
        }
        surfaceProfile.assign(river.centerline.size(), 0.0f);
        // Уклон — геометрический: весь перепад от истока до края мира,
        // размазанный по длине пути. Река спускается плавно и приходит к
        // устью ровно на уровень обрыва (kVoidHeight), а не проваливается
        // намного ниже него (тогда у самого края образуется ступенька, за
        // которой вода копится озером вместо того, чтобы падать в пустоту).
        const std::size_t headIdx = index(river.centerline.front().x, river.centerline.front().y);
        const float headCeiling = elevation[headIdx] - kRiverFreeboard;
        const float steps = static_cast<float>(std::max<std::size_t>(1, river.centerline.size() - 1));
        const float perSample = std::max(kRiverBedSlope, (headCeiling - kVoidHeight) / steps);
        for (std::size_t s = 0; s < river.centerline.size(); ++s) {
            const std::size_t centerIdx = index(river.centerline[s].x, river.centerline[s].y);
            // Земля минус запас на берег — вода нигде не стоит вровень с
            // окрестностью. min() с riverSurface оставляет слияние точным:
            // приток садится ровно на поверхность целевой реки, а не на
            // берег ниже неё.
            const float ceiling =
                std::min(elevation[centerIdx] - kRiverFreeboard, riverSurface[centerIdx]);
            const float descended = s == 0 ? ceiling : std::min(ceiling, surfaceProfile[s - 1] - perSample);
            // Ниже края мира река не опускается: там уже пустота.
            surfaceProfile[s] = std::max(descended, static_cast<float>(kVoidHeight));
        }
        for (const auto& [idx, cell] : river.footprint) {
            const std::size_t i = static_cast<std::size_t>(idx);
            const float surface = surfaceProfile[static_cast<std::size_t>(cell.sampleIndex)];
            // Глубина у берега стремится к нулю, в середине — к
            // params.riverDepth: это форма ДНА под ровной поверхностью.
            const float depth = static_cast<float>(params.riverDepth) * cell.falloff * cell.depthMul;
            elevation[i] = std::min(elevation[i], surface - depth);
            riverSurface[i] = std::min(riverSurface[i], surface);
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
    // области впадин глубже kMinPondDepth. Размер области ничем не
    // ограничивается — какой рельеф насчитал, такой пруд и получился.
    std::vector<bool> pondCandidate(cellCount, false);
    for (std::size_t i = 0; i < cellCount; ++i) {
        pondCandidate[i] = (filled[i] - elevation[i]) > kMinPondDepth;
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

        ++stats.pondComponentsPlaced;
        // Форма впадины (Priority-Flood) внутри пруда неровная — центр
        // глубже краёв. Нормируем её на среднюю глубину впадины по всему
        // пруду и умножаем на params.pondDepth, поэтому итоговая СРЕДНЯЯ
        // глубина воды по пруду равна ровно params.pondDepth (те же
        // единицы — глубина тайла, — что и у params.riverDepth), а форма
        // впадины сохраняется как внутренний узор глубины.
        float basinDepthSum = 0.0f;
        for (int idx : componentBuffer) {
            const std::size_t i = static_cast<std::size_t>(idx);
            basinDepthSum += filled[i] - elevation[i];
        }
        const float avgBasinDepth = basinDepthSum / static_cast<float>(componentBuffer.size());
        for (int idx : componentBuffer) {
            const std::size_t i = static_cast<std::size_t>(idx);
            const float basinDepth = filled[i] - elevation[i];
            const float shape = avgBasinDepth > 0.0f ? basinDepth / avgBasinDepth : 1.0f;
            waterDepth[i] = std::max(waterDepth[i], params.pondDepth * shape);
            // Дно опускается вместе с глубиной, а не остаётся на исходном
            // уровне рельефа: без этого поверхность воды (elevation +
            // waterDepth) оказалась бы выше естественной точки перелива
            // filled (пруд как будто раздувается сам из себя). Опускаем
            // дно так, чтобы поверхность осталась на filled — котловина
            // настоящая, а не вода поверх невыкопанного рельефа.
            elevation[i] = filled[i] - waterDepth[i];
        }
    }
    stats.pondMs = elapsedMs(pondStart);

    // --- 4. Реки: глубина воды по вырезанным руслам ---
    // Глубина — не самостоятельное число, а разница между уровнем
    // поверхности реки (посчитан выше) и дном: дно уже опущено ровно так,
    // чтобы эта разница дала нужный профиль, а поверхность поперёк русла
    // осталась ровной. Если пруд или другая река опустили дно ещё ниже,
    // глубина здесь просто получится больше — поверхность от этого не
    // поднимется.
    //
    // После прудов — на пересечении реки с прудом глубина берётся как max
    // (как и для двух прудов выше). Отдельного признака "здесь река" тайл
    // не получает: для симуляции реки и пруда не существует как разных
    // вещей, вода везде течёт по одному закону — уклону поверхности
    // (HydrologySystem).
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (std::isinf(riverSurface[i])) {
            continue;
        }
        waterDepth[i] = std::max(waterDepth[i], std::max(0.0f, riverSurface[i] - elevation[i]));
    }

    // --- 4b. Источники воды: РОВНО ОДИН на реку, у самого её истока в
    // горах, плюс waterSourceCount "родников" в случайных точках карты
    // (docs/01_Cosmology.md). Отмечаются здесь, ДО расчёта влажности ниже,
    // — источник сразу стоит полным столбом и участвует в distanceToWater
    // наравне с любой другой водой, а не наливается десятки тиков.
    //
    // Источник — не приток известной мощности, а столб воды постоянной
    // глубины: HydrologySystem каждый тик просто ставит его глубину
    // обратно на params.waterSourceDepth, сколько бы из него ни вытекло.
    // Поэтому решает не глубина столба, а ЧИСЛО источников: когда источник
    // стоял на каждой клетке выше ледниковой отметки, их выходили сотни, и
    // карту заливало целиком.
    std::vector<bool> isWaterSource(cellCount, false);
    for (const auto& river : acceptedRivers) {
        if (!river.centerline.empty()) {
            const auto& head = river.centerline.front();
            isWaterSource[index(head.x, head.y)] = true;
        }
    }
    {
        std::mt19937 sourceRng(seed + 20);
        std::uniform_int_distribution<int> xDist(0, width - 1);
        std::uniform_int_distribution<int> yDist(0, height - 1);
        for (int n = 0; n < params.waterSourceCount; ++n) {
            isWaterSource[index(xDist(sourceRng), yDist(sourceRng))] = true;
        }
    }
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (isWaterSource[i]) {
            waterDepth[i] = std::max(waterDepth[i], static_cast<float>(params.waterSourceDepth));
            ++stats.waterSourcesPlaced;
        }
    }

    // --- 4c. Край мира: обрыв в пустоту (docs/01_Cosmology.md). Нулевая
    // высота, никакой воды — и последним, после всех рек, прудов и
    // источников, чтобы ни одна из этих стадий не могла оставить на самой
    // границе ступеньку или лужу. HydrologySystem держит то же условие
    // каждый тик.
    for (int x = 0; x < width; ++x) {
        for (int y : {0, height - 1}) {
            const std::size_t i = index(x, y);
            elevation[i] = kVoidHeight;
            waterDepth[i] = 0.0f;
            isWaterSource[i] = false;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x : {0, width - 1}) {
            const std::size_t i = index(x, y);
            elevation[i] = kVoidHeight;
            waterDepth[i] = 0.0f;
            isWaterSource[i] = false;
        }
    }

    // --- 5. Влажность: равновесное состояние (core/Moisture.hpp) ---
    // Multi-source BFS от всех водных клеток — стандартный distance
    // transform, даёт плавный градиент вместо ступенчатого. Дальше та же
    // функция moistureTarget, к которой влажность тянет и HydrologySystem
    // каждый тик: мир генерируется сразу в равновесии, и первый же тик
    // симуляции не начинает переписывать только что созданную карту.
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

            const int soilRockiness = std::clamp(static_cast<int>(std::lround(rockiness[i] * kFull)), 0, kFull);
            const int moisture = moistureTarget(distanceToWater[i], soilRockiness);

            // Минералы: шум, в среднем дающий params.mineralsAverage
            // (noise01 в среднем ~0.5, поэтому *2*mineralsAverage сходится
            // к среднему mineralsAverage). Отдельного значения для речных
            // клеток нет: минералы разносит по карте течение
            // (HydrologySystem, правило песочной кучи), и русло получает
            // своё содержание само, а не назначением при генерации.
            const float mineralsNoise01 = normalize01(mineralsNoise.GetNoise(static_cast<float>(x), static_cast<float>(y)));
            const int minerals =
                std::max(0, static_cast<int>(std::lround(mineralsNoise01 * params.mineralsAverage * 2.0f)));
            // Дробные слои генерации становятся целым состоянием мира
            // ровно здесь, на записи в компонент: внутри самой генерации
            // считать дробями и удобно, и правильно — это способ получить
            // мир, а не сам мир (core/Scale.hpp).
            const auto entity = world.registry().create();
            world.registry().emplace<SoilComponent>(entity, SoilComponent{moisture, soilRockiness, minerals});
            world.registry().emplace<HeightComponent>(
                entity, HeightComponent{static_cast<int>(std::lround(elevation[i]))});
            const int depth = static_cast<int>(std::lround(waterDepth[i]));
            if (depth > 0) {
                world.registry().emplace<WaterComponent>(entity, WaterComponent{depth, 0});
            }
            if (isWaterSource[i]) {
                world.registry().emplace<WaterSourceComponent>(entity);
            }
            world.place(entity, x, y);
        }
    }
    stats.entityMs = elapsedMs(entityStart);

    // --- Свойства мира: выбираются один раз здесь, дальше System-ы (в
    // частности HydrologySystem) их только читают (06_GameLoop.md,
    // п.1a). WorldPropertiesComponent на World Entity уже существует
    // (создан в World::reset/конструкторе) — значения по умолчанию
    // просто перезаписываются выбором этой генерации.
    auto& worldProperties = world.registry().get<WorldPropertiesComponent>(world.worldEntity());
    worldProperties.waterSourceDepth = params.waterSourceDepth;
    worldProperties.waterEvaporationRate = params.waterEvaporationRate;
    worldProperties.rainIntervalTicks = params.rainIntervalTicks;
    worldProperties.rainAmount = params.rainAmount;
    worldProperties.soilErosionRate = params.soilErosionRate;
    worldProperties.mineralsSpreadEnabled = params.mineralsSpreadEnabled;

    stats.totalMs = elapsedMs(totalStart);
    return stats;
}


// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
// Список ведётся здесь же, под самими значениями: добавил константу —
// добавь строку сюда, и она сама появится в оверлее клиента.
void appendTerrainConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Terrain";
    out.push_back({g, "kNoiseLacunarity", kNoiseLacunarity});
    out.push_back({g, "kNoiseGain", kNoiseGain});
    out.push_back({g, "kRockFrequencyRatio", kRockFrequencyRatio});
    out.push_back({g, "kMineralsFrequencyRatio", kMineralsFrequencyRatio});
    out.push_back({g, "kMeanderFrequency", kMeanderFrequency});
    out.push_back({g, "kMeanderPushScale", kMeanderPushScale});
    out.push_back({g, "kSoilProbeDist", kSoilProbeDist});
    out.push_back({g, "kSoilPushScale", kSoilPushScale});
    out.push_back({g, "kSoilPushClamp", kSoilPushClamp});
    out.push_back({g, "kMaxLateralPerStep", kMaxLateralPerStep});
    out.push_back({g, "kMaxWander", kMaxWander});
    out.push_back({g, "kWidthNoiseFrequency", kWidthNoiseFrequency});
    out.push_back({g, "kWidthNoiseAmplitude", kWidthNoiseAmplitude});
    out.push_back({g, "kWidthMinMul", kWidthMinMul});
    out.push_back({g, "kWidthMaxMul", kWidthMaxMul});
    out.push_back({g, "kDepthNoiseFrequency", kDepthNoiseFrequency});
    out.push_back({g, "kDepthNoiseAmplitude", kDepthNoiseAmplitude});
    out.push_back({g, "kDepthMinMul", kDepthMinMul});
    out.push_back({g, "kDepthMaxMul", kDepthMaxMul});
    out.push_back({g, "kRiverAttemptMultiplier", static_cast<float>(kRiverAttemptMultiplier)});
    out.push_back({g, "kMergeProbability", kMergeProbability});
    out.push_back({g, "kMergeMarginFraction", kMergeMarginFraction});
    out.push_back({g, "kRiverBedSlope", kRiverBedSlope});
    out.push_back({g, "kRiverFreeboard", kRiverFreeboard});
    out.push_back({g, "kVoidHeight", kVoidHeight});
    out.push_back({g, "kMountainSharpness", kMountainSharpness});
    out.push_back({g, "kRiverHeadTopFraction", kRiverHeadTopFraction});
    out.push_back({g, "kRiverHeadPeakSkipFraction", kRiverHeadPeakSkipFraction});
    out.push_back({g, "kMinPondDepth", kMinPondDepth});
    out.push_back({g, "kRiverStageDeadlineMs", static_cast<float>(kRiverStageDeadlineMs)});
    out.push_back({g, "kMaxRiverPathSamples", static_cast<float>(kMaxRiverPathSamples)});
}

} // namespace goblins

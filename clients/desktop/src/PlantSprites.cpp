#include "PlantSprites.hpp"

#include <array>
#include <cmath>
#include <vector>

#include "SpriteAtlas.hpp"
#include "TileColors.hpp"

namespace PlantSprites {

namespace {

// Имена кадров в файлах рисунка. Порядок здесь — порядок возрастов, а не
// порядок строк в файле: кадры ищутся по имени (Assets::frameIndex), поэтому
// переставить их в ресурсе местами можно, а переименовать — нельзя, и вот
// эти списки тому единственная причина.
constexpr std::array<const char*, kStages * kGrassVariants * kFrames> kGrassFrames = {
    "grass.sprout.0.a", "grass.sprout.0.b", "grass.sprout.1.a", "grass.sprout.1.b",
    "grass.sprout.2.a", "grass.sprout.2.b", "grass.low.0.a",    "grass.low.0.b",
    "grass.low.1.a",    "grass.low.1.b",    "grass.low.2.a",    "grass.low.2.b",
    "grass.tall.0.a",   "grass.tall.0.b",   "grass.tall.1.a",   "grass.tall.1.b",
    "grass.tall.2.a",   "grass.tall.2.b",
};

// У куста к возрастам добавлены ягоды — двумя кадрами поверх, а не удвоением
// всех остальных.
constexpr std::array<const char*, kStages * kFrames + 2> kBushFrames = {
    "bush.sprout.a", "bush.sprout.b", "bush.young.a",  "bush.young.b",
    "bush.grown.a",  "bush.grown.b",  "berries.few",   "berries.many",
};
constexpr int kBerriesFew = kStages * kFrames;
constexpr int kBerriesMany = kBerriesFew + 1;

// Комель куста. Та же кора, что у дерева, и по той же причине, по которой у
// деревьев она одна на все виды: вид опознаётся листвой, и отдать коре вторую
// работу значило бы отнять у листвы единственную.
constexpr Color kStem{58, 44, 32, 255};

// Ягода — тот же красный, каким ягодник светится в самой текстуре карты
// (TileColors::berries). Переход между "далеко" и "близко" не должен
// выглядеть сменой предмета.
constexpr Color kBerry{188, 62, 74, 255};

Color darken(Color color, float amount) {
    return Color{static_cast<unsigned char>(color.r * (1.0f - amount)),
                 static_cast<unsigned char>(color.g * (1.0f - amount)),
                 static_cast<unsigned char>(color.b * (1.0f - amount)), color.a};
}

// Раскраска на вид. Возраст цвет не меняет — его говорит рисунок, — и это то
// же правило, что у дерева: у цвета одна работа, сказать, какой это вид.
SpriteAtlas::Palette grassPalette(int species) {
    const Color blade = TileColors::plantSpecies(species);
    return {SpriteAtlas::Ink{'G', blade}, SpriteAtlas::Ink{'g', darken(blade, 0.35f)}};
}

SpriteAtlas::Palette bushPalette(int species) {
    const Color leaf = TileColors::bushSpecies(species);
    return {SpriteAtlas::Ink{'B', leaf}, SpriteAtlas::Ink{'b', darken(leaf, 0.35f)},
            SpriteAtlas::Ink{'t', kStem}, SpriteAtlas::Ink{'R', kBerry}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не хватает
// хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): куст, показанный
// ростком, хуже, чем куст, показанный оттенком клетки, потому что выглядит
// как ответ, а отвечает неверно.
const SpriteAtlas::Detailed& grassBaked() {
    static const SpriteAtlas::Detailed result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kGrassSpeciesCount);
        for (int species = 0; species < TileColors::kGrassSpeciesCount; ++species) {
            palettes.push_back(grassPalette(species));
        }
        return SpriteAtlas::bakeDetailed("grass", palettes, kGrassFrames);
    }();
    return result;
}

const SpriteAtlas::Detailed& bushBaked() {
    static const SpriteAtlas::Detailed result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kBushSpeciesCount);
        for (int species = 0; species < TileColors::kBushSpeciesCount; ++species) {
            palettes.push_back(bushPalette(species));
        }
        return SpriteAtlas::bakeDetailed("bush", palettes, kBushFrames);
    }();
    return result;
}

// Номера вне пределов заворачиваются остатком, а не падают: возраст с
// номером больше, чем ступеней, — вопрос к тому, кто его посчитал, а не повод
// уронить клиент (то же правило, что и в SpriteAtlas::Placement::source).
int frameAt(int stage, int frame) {
    const int s = stage < 0 ? 0 : stage % kStages;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return s * kFrames + f;
}

} // namespace

Detail grassDetailFor(float tileSize) {
    return grassBaked().detailFor(tileSize);
}

Detail bushDetailFor(float tileSize) {
    return bushBaked().detailFor(tileSize);
}

bool grassReady(Detail detail) {
    return grassBaked().ready(detail);
}

bool bushReady(Detail detail) {
    return bushBaked().ready(detail);
}

const Texture2D& grassAtlas(Detail detail) {
    return grassBaked().texture(detail);
}

const Texture2D& bushAtlas(Detail detail) {
    return bushBaked().texture(detail);
}

Rectangle grass(Detail detail, int species, int stage, int variant, int frame) {
    const int s = stage < 0 ? 0 : stage % kStages;
    const int v = variant < 0 ? 0 : variant % kGrassVariants;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return grassBaked().sheet(detail).source(species, (s * kGrassVariants + v) * kFrames + f);
}

int variantOf(int x, int y) {
    // Множители не те же, что у фазы качания (frameOf), и это важно: совпади
    // они, кустик и такт качания менялись бы вместе, и луг разбился бы на
    // косые полосы вместо ровной пестроты.
    return ((x * 3 + y * 7) % kGrassVariants + kGrassVariants) % kGrassVariants;
}

Rectangle bush(Detail detail, int species, int stage, int frame) {
    return bushBaked().sheet(detail).source(species, frameAt(stage, frame));
}

Rectangle berries(Detail detail, int count) {
    // Половина полного куста — граница между "горсть" и "полон"
    // (TileColors::kBerriesVisualCap, оно же kBerryMax мира). Ступеней две, а
    // не шкала: на клетке в шестнадцать пикселей разница между семью ягодами
    // и восемью не видна никакому глазу, а между горстью и полным кустом —
    // видна, и ровно её и спрашивают, глядя на карту.
    const bool many = static_cast<float>(count) >= TileColors::kBerriesVisualCap * 0.5f;
    return bushBaked().sheet(detail).source(0, many ? kBerriesMany : kBerriesFew);
}

int stageOf(float growth) {
    if (growth < 0.30f) {
        return 0;
    }
    if (growth < 0.70f) {
        return 1;
    }
    return 2;
}

int frameOf(int x, int y, double seconds) {
    // Полсекунды на кадр — то же, что у дерева: быстрее трава дрожит,
    // медленнее качание перестаёт читаться как движение.
    constexpr double kFrameSeconds = 0.5;
    // Фаза от клетки, и множители нарочно другие, чем у дерева: совпади они,
    // трава качалась бы в такт с деревом, стоящим на той же клетке, и обе
    // выглядели бы одним предметом.
    const double phase = static_cast<double>((x * 11 + y * 5) % 7) / 7.0;
    const double step = seconds / kFrameSeconds + phase;
    return static_cast<int>(std::floor(step)) & 1;
}

} // namespace PlantSprites

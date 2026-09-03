#include "TreeSprites.hpp"

#include <array>
#include <cmath>
#include <vector>

#include "Assets.hpp"
#include "SpriteAtlas.hpp"
#include "TileColors.hpp"

namespace TreeSprites {

namespace {

// Имена кадров в файле рисунка. Порядок здесь — порядок возрастов, а не
// порядок строк в файле: кадры ищутся по имени (Assets::frameIndex),
// поэтому переставить их в ресурсе местами можно, а переименовать —
// нельзя, и вот этот список тому единственная причина.
constexpr std::array<const char*, kStages * kVariants * kFrames> kFrameNames = {
    "sprout.0.a", "sprout.0.b", "sprout.1.a", "sprout.1.b", "sprout.2.a", "sprout.2.b",
    "young.0.a",  "young.0.b",  "young.1.a",  "young.1.b",  "young.2.a",  "young.2.b",
    "mature.0.a", "mature.0.b", "mature.1.a", "mature.1.b", "mature.2.a", "mature.2.b",
};

// Кора у всех видов одна: ствол — это ствол, а вид опознаётся кроной.
// Разводить кору по видам значило бы отнять у кроны единственную работу,
// которую она делает.
constexpr Color kBark{58, 44, 32, 255};
constexpr Color kBarkLit{78, 60, 42, 255};

// Тень на земле у комля — та же, что под зверем и гоблином, и по той же
// причине: полупрозрачная, чтобы одинаково лечь и на светлый песок, и на
// траву. Дерево от неё перестаёт висеть в воздухе.
constexpr Color kShadow{16, 22, 18, 105};

Color lighten(Color color, float amount) {
    const auto up = [&](unsigned char v) {
        return static_cast<unsigned char>(v + (255 - v) * amount);
    };
    return Color{up(color.r), up(color.g), up(color.b), color.a};
}

Color darken(Color color, float amount) {
    return Color{static_cast<unsigned char>(color.r * (1.0f - amount)),
                 static_cast<unsigned char>(color.g * (1.0f - amount)),
                 static_cast<unsigned char>(color.b * (1.0f - amount)), color.a};
}

// Раскраска на вид дерева. Возраст цвет не меняет: его говорит силуэт —
// росток, подрост и взрослое отличаются рисунком, а не оттенком, — и
// оставить цвету одну работу (какой это вид) вернее, чем нагрузить его
// двумя.
SpriteAtlas::Palette paletteOf(int species) {
    const Color crown = TileColors::treeSpecies(species);
    // Тонов кроны три, а было два, и это главное, что делает дерево деревом,
    // а не зелёным пятном: свет падает сверху слева, и от того у каждого кома
    // листвы виден верх и виден испод. Тем же тоном разделены комья и внутри
    // кроны — без этого она плоская.
    return {SpriteAtlas::Ink{'H', lighten(crown, 0.28f)}, SpriteAtlas::Ink{'C', crown},
            SpriteAtlas::Ink{'c', darken(crown, 0.38f)},  SpriteAtlas::Ink{'T', kBarkLit},
            SpriteAtlas::Ink{'t', kBark},                 SpriteAtlas::Ink{'d', kShadow}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не
// хватает хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): дерево
// без своего возраста хуже, чем дерево прямоугольником, потому что
// выглядит как дерево не того возраста.
const SpriteAtlas::Detailed& baked() {
    static const SpriteAtlas::Detailed result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kTreeSpeciesCount);
        for (int species = 0; species < TileColors::kTreeSpeciesCount; ++species) {
            palettes.push_back(paletteOf(species));
        }
        return SpriteAtlas::bakeDetailed("tree", palettes, kFrameNames);
    }();
    return result;
}

} // namespace

Detail detailFor(float tileSize) {
    return baked().detailFor(tileSize);
}

bool ready(Detail detail) {
    return baked().ready(detail);
}

const Texture2D& atlas(Detail detail) {
    return baked().texture(detail);
}

Rectangle source(Detail detail, int species, int stage, int variant, int frame) {
    const int s = stage < 0 ? 0 : stage % kStages;
    const int v = variant < 0 ? 0 : variant % kVariants;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return baked().sheet(detail).source(species, (s * kVariants + v) * kFrames + f);
}

int variantOf(int x, int y) {
    return ((x * 5 + y * 3) % kVariants + kVariants) % kVariants;
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
    // Сколько держится один кадр. Полсекунды: быстрее — дерево дрожит,
    // медленнее — качание перестаёт читаться как движение.
    constexpr double kFrameSeconds = 0.5;
    // Фаза от клетки, а не от номера дерева: номер меняется при загрузке
    // мира, а клетка — нет, и роща не должна перестраивать своё качание от
    // того, что мир открыли заново.
    const double phase = static_cast<double>((x * 7 + y * 13) % 5) / 5.0;
    const double step = seconds / kFrameSeconds + phase;
    return static_cast<int>(std::floor(step)) & 1;
}

} // namespace TreeSprites

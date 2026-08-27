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
constexpr std::array<const char*, kStages * kFrames> kFrameNames = {
    "sprout.a", "sprout.b", "young.a", "young.b", "mature.a", "mature.b",
};

// Кора у всех видов одна: ствол — это ствол, а вид опознаётся кроной.
// Разводить кору по видам значило бы отнять у кроны единственную работу,
// которую она делает.
constexpr Color kBark{58, 44, 32, 255};
constexpr Color kBarkLit{78, 60, 42, 255};

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
    return {SpriteAtlas::Ink{'C', crown}, SpriteAtlas::Ink{'c', darken(crown, 0.30f)},
            SpriteAtlas::Ink{'T', kBarkLit}, SpriteAtlas::Ink{'t', kBark}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не
// хватает хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): дерево
// без своего возраста хуже, чем дерево прямоугольником, потому что
// выглядит как дерево не того возраста.
const SpriteAtlas::Baked& baked() {
    static const SpriteAtlas::Baked result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kTreeSpeciesCount);
        for (int species = 0; species < TileColors::kTreeSpeciesCount; ++species) {
            palettes.push_back(paletteOf(species));
        }
        return SpriteAtlas::bake("tree", palettes, kFrameNames);
    }();
    return result;
}

} // namespace

bool ready() {
    return baked().complete;
}

const Texture2D& atlas() {
    return baked().sheet.texture();
}

Rectangle source(int species, int stage, int frame) {
    const int clampedStage = stage < 0 ? 0 : stage % kStages;
    const int clampedFrame = frame < 0 ? 0 : frame % kFrames;
    return baked().source(species, clampedStage * kFrames + clampedFrame);
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

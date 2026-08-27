#include "BuildSprites.hpp"

#include <array>

#include "SpriteAtlas.hpp"

namespace BuildSprites {

namespace {

// Имена кадров в файле рисунка. Порядок здесь — порядок, в котором их
// спрашивают отсюда же (см. kCanopy ниже), а не порядок строк в ресурсе:
// кадры ищутся по имени, поэтому переставить их в файле можно, а
// переименовать — нельзя, и вот этот список тому единственная причина.
constexpr std::array<const char*, 9> kFrameNames = {
    "canopy.poles",   "canopy.half", "canopy.full", "bedding.straws", "bedding.half",
    "bedding.full",   "site.canopy", "site.bedding", "material",
};

constexpr int kCanopy = 0;
constexpr int kBedding = kCanopy + kStages;
constexpr int kSite = kBedding + kStages;
constexpr int kMaterial = kSite + 2;

// Раскраска одна на все постройки, и это не упущение, а закон мира: навес
// принадлежит МЕСТУ, а не племени (BuildingComponent). Красить его по тому,
// кто строил, значило бы завести в мире собственность, которой в нём нет ни
// в одном законе.
//
// Цвета — той же семьи, что и оттенки построек в самой текстуре карты
// (TileColors::canopy, ::bedding, ::site): на мелком масштабе рисунка нет и
// клетка красится оттенком, и переход между "далеко" и "близко" не должен
// выглядеть сменой предмета.
SpriteAtlas::Palette palette() {
    return {
        // Солома крыши: тёплая, светлее земли — навес видно первым.
        SpriteAtlas::Ink{'R', Color{198, 164, 96, 255}},
        SpriteAtlas::Ink{'r', Color{146, 116, 64, 255}},
        // Жердь: та же кора, что у дерева, — её оттуда и принесли.
        SpriteAtlas::Ink{'P', Color{104, 80, 54, 255}},
        SpriteAtlas::Ink{'p', Color{72, 55, 38, 255}},
        // Подстилка светлее крыши: она лежит на виду, а не в тени под ней.
        SpriteAtlas::Ink{'B', Color{206, 190, 138, 255}},
        SpriteAtlas::Ink{'b', Color{166, 148, 102, 255}},
        // Колышек замысла — холодный, как и крапина площадки на карте:
        // задуманное не должно спорить цветом со сделанным.
        SpriteAtlas::Ink{'S', Color{132, 162, 184, 255}},
        // Материал: солома внавал и ветки поперёк неё.
        SpriteAtlas::Ink{'M', Color{186, 168, 110, 255}},
        SpriteAtlas::Ink{'m', Color{96, 74, 50, 255}},
    };
}

const SpriteAtlas::Baked& baked() {
    static const SpriteAtlas::Baked result = [] {
        const std::array<SpriteAtlas::Palette, 1> palettes = {palette()};
        return SpriteAtlas::bake("build", palettes, kFrameNames);
    }();
    return result;
}

int clampStage(int stage) {
    return stage < 0 ? 0 : stage % kStages;
}

} // namespace

bool ready() {
    return baked().complete;
}

const Texture2D& atlas() {
    return baked().sheet.texture();
}

Rectangle canopy(int stage) {
    return baked().source(0, kCanopy + clampStage(stage));
}

Rectangle bedding(int stage) {
    return baked().source(0, kBedding + clampStage(stage));
}

Rectangle site(int kind) {
    // Числа те же, что в слое "site" протокола (1 навес, 2 подстилка).
    // Незнакомый вид рисуется колышками навеса, а не остаётся невидимым:
    // площадка, которую не видно, — худшее из возможного, ведь именно её и
    // ищут глазами, когда спрашивают "почему они там толкутся".
    return baked().source(0, kSite + (kind == 2 ? 1 : 0));
}

Rectangle material() {
    return baked().source(0, kMaterial);
}

int stageOf(float condition) {
    // Границы те же, что у возрастов дерева, и это не совпадение: ступень
    // должна читаться с одного взгляда, а треть шкалы — самая мелкая
    // разница, которую глаз ловит без разглядывания.
    if (condition < 0.30f) {
        return 0;
    }
    if (condition < 0.70f) {
        return 1;
    }
    return 2;
}

} // namespace BuildSprites

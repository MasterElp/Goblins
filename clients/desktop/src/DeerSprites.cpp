#include "DeerSprites.hpp"

#include <array>
#include <vector>

#include "SpriteAtlas.hpp"
#include "TileColors.hpp"

namespace DeerSprites {

namespace {

// Имена кадров в файле рисунка. Порядок здесь — порядок, в котором их
// спрашивают отсюда же (frameAt ниже): облик, занятие, кадр. В самом ресурсе
// он может быть любым — кадры ищутся по имени (Assets::frameIndex), и вот
// этот список тому единственная причина.
//
// Телёнок первым, а за ним взрослые: тем же порядком идут ступени у дерева, у
// гоблина и у волка, и возвращает его kindOf.
constexpr std::array<const char*, kKinds * kPoses * kFrames> kFrameNames = {
    "fawn.stand.a", "fawn.stand.b", "fawn.walk.a", "fawn.walk.b",
    "fawn.flee.a",  "fawn.flee.b",  "fawn.feed.a", "fawn.feed.b",

    "doe.stand.a",  "doe.stand.b",  "doe.walk.a",  "doe.walk.b",
    "doe.flee.a",   "doe.flee.b",   "doe.feed.a",  "doe.feed.b",

    "stag.stand.a", "stag.stand.b", "stag.walk.a", "stag.walk.b",
    "stag.flee.a",  "stag.flee.b",  "stag.feed.a", "stag.feed.b",
};

// Тень на земле — та же, что под волком и гоблином, и по той же причине:
// обводки нет, а зверь не должен теряться на пёстрой земле.
constexpr Color kShadow{16, 22, 18, 105};
// Глаз — самое тёмное на фигуре: он один говорит, куда зверь повёрнут.
constexpr Color kEyeDark{34, 26, 20, 255};
// Рога — сухая ветвь, и цвет у них ветвяной: та же кора, что у дерева и у
// комля куста. Одинаковый у всех видов, как и у гоблина повязка: вид
// опознаётся шерстью, и отдать рогам вторую работу значило бы отнять у шерсти
// единственную.
constexpr Color kAntler{104, 82, 58, 255};

Color darken(Color color, float amount) {
    return Color{static_cast<unsigned char>(color.r * (1.0f - amount)),
                 static_cast<unsigned char>(color.g * (1.0f - amount)),
                 static_cast<unsigned char>(color.b * (1.0f - amount)), color.a};
}

Color lighten(Color color, float amount) {
    const auto up = [&](unsigned char v) {
        return static_cast<unsigned char>(v + (255 - v) * amount);
    };
    return Color{up(color.r), up(color.g), up(color.b), color.a};
}

// Раскраска на вид. Тонов шерсти три, и это то же, чем читается волк: зверь
// на клетке слишком мал, чтобы сказать формой, где спина, где брюхо, где
// дальняя пара ног, — за него это говорит тон.
SpriteAtlas::Palette paletteOf(int species) {
    const Color fur = TileColors::herbivoreSpecies(species);
    return {SpriteAtlas::Ink{'H', lighten(fur, 0.26f)}, SpriteAtlas::Ink{'S', fur},
            SpriteAtlas::Ink{'s', darken(fur, 0.42f)},  SpriteAtlas::Ink{'A', kAntler},
            SpriteAtlas::Ink{'d', kShadow},             SpriteAtlas::Ink{'E', kEyeDark}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не хватает
// хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): олень, стоящий
// вместо того, чтобы бежать, хуже кружка, потому что выглядит как ответ, а
// отвечает неверно.
const SpriteAtlas::Baked& baked() {
    static const SpriteAtlas::Baked result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kHerbivoreSpeciesCount);
        for (int species = 0; species < TileColors::kHerbivoreSpeciesCount; ++species) {
            palettes.push_back(paletteOf(species));
        }
        return SpriteAtlas::bake("deer", palettes, kFrameNames);
    }();
    return result;
}

// Номера вне пределов заворачиваются остатком, а не падают: облик с номером
// больше, чем их есть, — вопрос к тому, кто его посчитал, а не повод уронить
// клиент (то же правило, что и в SpriteAtlas::Sheet::source).
int frameAt(Pose pose, int kind, int frame) {
    const int p = static_cast<int>(pose) < 0 ? 0 : static_cast<int>(pose) % kPoses;
    const int k = kind < 0 ? 0 : kind % kKinds;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return (k * kPoses + p) * kFrames + f;
}

} // namespace

bool ready() {
    return baked().complete;
}

const Texture2D& atlas() {
    return baked().sheet.texture();
}

Rectangle source(int species, Pose pose, int kind, int frame, int facing) {
    Rectangle piece = baked().source(species, frameAt(pose, kind, frame));
    if (facing < 0) {
        // Отрицательная ширина — то, как raylib просят отразить кусок по
        // горизонтали. Зверь нарисован во всю ширину кадра, от хвоста до
        // морды, поэтому отражение не сдвигает его внутри клетки.
        piece.width = -piece.width;
    }
    return piece;
}

int kindOf(float growth, const std::string& sex) {
    // 0.90 — kBreedingGrowth из AnimalSystem, порог взрослости мира. Число
    // повторено, а не вынесено: клиент не подключает ядро вовсе
    // (07_TechStack.md, п.6), и приезжает к нему не порог, а развитость.
    if (growth < 0.90f) {
        return 0; // телёнок: рогов нет ни при каком поле
    }
    return sex == "male" ? 2 : 1;
}

Pose poseOf(const std::string& desire, bool walking) {
    if (walking) {
        // Бегство — единственное желание, которому нужен и шаг: оно и ЕСТЬ
        // шаг, и стоящий на месте беглец не выглядит ничем.
        return desire == "flee" ? Pose::Flee : Pose::Walk;
    }
    // Дальше он стоит на месте, и желание говорит, что он там делает.
    // Еда и вода одним кадром: со стороны и то, и другое — морда у земли.
    if (desire == "food" || desire == "water") {
        return Pose::Feed;
    }
    // Сюда попадают "idle", "mate" и всякое желание, которого в мире ещё не
    // было, когда рисовались кадры.
    return Pose::Stand;
}

bool walkingNow(std::uint64_t stepTick, std::uint64_t tick) {
    constexpr std::uint64_t kWalkMemory = 5;
    // Ноль — шага не видели ни разу: зверь только что родился или мир только
    // что открыт. Стоит, а не идёт: первый же его шаг это исправит.
    if (stepTick == 0 || tick < stepTick) {
        return false;
    }
    return tick - stepTick <= kWalkMemory;
}

int frameOf(std::uint64_t id, std::uint64_t tick) {
    // Два тика на кадр, а не один, как у волка: травоядное медленнее, и
    // походка у него должна быть спокойнее — иначе пасущееся стадо выглядит
    // мечущимся.
    constexpr std::uint64_t kTicksPerFrame = 2;
    constexpr std::uint64_t kCycle = kTicksPerFrame * kFrames;
    return static_cast<int>(((tick + id % kCycle) / kTicksPerFrame) % kFrames);
}

} // namespace DeerSprites

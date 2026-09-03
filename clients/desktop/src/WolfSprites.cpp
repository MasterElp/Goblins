#include "WolfSprites.hpp"

#include <array>
#include <vector>

#include "SpriteAtlas.hpp"
#include "TileColors.hpp"

namespace WolfSprites {

namespace {

// Имена кадров в файле рисунка. Порядок здесь — порядок, в котором их
// спрашивают отсюда же (frameAt ниже): возраст, занятие, кадр. В самом
// ресурсе он может быть любым — кадры ищутся по имени (Assets::frameIndex),
// и вот этот список тому единственная причина.
//
// Щенок первым, а не взрослый: тем же порядком идут ступени у дерева и у
// гоблина, и возвращает его stageOf.
constexpr std::array<const char*, kStages * kPoses * kFrames> kFrameNames = {
    "pup.stand.a",   "pup.stand.b",   "pup.walk.a",   "pup.walk.b",
    "pup.stalk.a",   "pup.stalk.b",   "pup.feed.a",   "pup.feed.b",

    "adult.stand.a", "adult.stand.b", "adult.walk.a", "adult.walk.b",
    "adult.stalk.a", "adult.stalk.b", "adult.feed.a", "adult.feed.b",
};

// Тень на земле — та же, что под гоблином, и по той же причине: обводки нет,
// а зверь не должен теряться на пёстрой земле. Полупрозрачная: под ней и
// светлый песок, и трава, и вода.
constexpr Color kShadow{16, 22, 18, 105};
// Глаз — самое тёмное на всей фигуре: он один говорит, куда зверь повёрнут.
constexpr Color kEyeDark{28, 22, 22, 255};

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

// Раскраска на вид. Тонов шерсти три, и это главное, чем кадр вообще
// читается: зверь на клетке — четырнадцать пикселей в длину и десять в
// высоту, и при одном тоне он выглядит пятном. Тон делит его на спину, бок и
// ноги там, где силуэту не хватает места сказать то же самое формой.
//
// Светлее именно спина — на неё падает свет, и с высоты глаза видно её
// первой; дальняя пара ног, наоборот, темнее всего, иначе четыре ноги
// читаются одним столбом.
SpriteAtlas::Palette paletteOf(int species) {
    const Color fur = TileColors::predatorSpecies(species);
    return {SpriteAtlas::Ink{'H', lighten(fur, 0.28f)}, SpriteAtlas::Ink{'S', fur},
            SpriteAtlas::Ink{'s', darken(fur, 0.45f)},  SpriteAtlas::Ink{'d', kShadow},
            SpriteAtlas::Ink{'E', kEyeDark}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не хватает
// хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): волк, стоящий
// вместо того, чтобы красться, хуже кружка, потому что выглядит как ответ, а
// отвечает неверно.
const SpriteAtlas::Detailed& baked() {
    static const SpriteAtlas::Detailed result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kPredatorSpeciesCount);
        for (int species = 0; species < TileColors::kPredatorSpeciesCount; ++species) {
            palettes.push_back(paletteOf(species));
        }
        return SpriteAtlas::bakeDetailed("wolf", palettes, kFrameNames);
    }();
    return result;
}

// Номера вне пределов заворачиваются остатком, а не падают: возраст с
// номером больше, чем ступеней, — вопрос к тому, кто его посчитал, а не повод
// уронить клиент (то же правило, что и в SpriteAtlas::Sheet::source).
int frameAt(Pose pose, int stage, int frame) {
    const int p = static_cast<int>(pose) < 0 ? 0 : static_cast<int>(pose) % kPoses;
    const int s = stage < 0 ? 0 : stage % kStages;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return (s * kPoses + p) * kFrames + f;
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

Rectangle source(Detail detail, int species, Pose pose, int stage, int frame, int facing) {
    Rectangle piece = baked().sheet(detail).source(species, frameAt(pose, stage, frame));
    if (facing < 0) {
        // Отрицательная ширина — то, как raylib просят отразить кусок по
        // горизонтали. Зверь нарисован во всю ширину кадра, от хвоста до
        // морды, поэтому отражение не сдвигает его внутри клетки.
        piece.width = -piece.width;
    }
    return piece;
}

int stageOf(float growth) {
    // 0.90 — kBreedingGrowth из AnimalSystem, порог взрослости мира. Число
    // повторено, а не вынесено: клиент не подключает ядро вовсе
    // (07_TechStack.md, п.6), и приезжает к нему не порог, а развитость.
    return growth < 0.90f ? 0 : 1;
}

Pose poseOf(const std::string& desire, bool walking) {
    if (walking) {
        // Идущий ЗА ЕДОЙ хищник охотится — ради этого одного различия зверю и
        // дан свой кадр, которого нет ни у кого больше на карте.
        return desire == "food" ? Pose::Stalk : Pose::Walk;
    }
    // Дальше он стоит на месте, и желание говорит, что он там делает.
    // Еда и вода одним кадром: со стороны и то, и другое — морда у земли, и
    // разводить их значило бы рисовать разницу, которой нет.
    if (desire == "food" || desire == "water") {
        return Pose::Feed;
    }
    // Сюда попадают "idle", "mate", "flee" и всякое желание, которого в мире
    // ещё не было, когда рисовались кадры. Стояние — честный ответ: бегство
    // на месте выглядит стоянием, потому что бегство — это шаг, а шага нет.
    return Pose::Stand;
}

bool walkingNow(std::uint64_t stepTick, std::uint64_t tick) {
    constexpr std::uint64_t kWalkMemory = 3;
    // Ноль — шага не видели ни разу: зверь только что родился или мир только
    // что открыт. Стоит, а не идёт: первый же его шаг это исправит.
    if (stepTick == 0 || tick < stepTick) {
        return false;
    }
    return tick - stepTick <= kWalkMemory;
}

int frameOf(std::uint64_t id, std::uint64_t tick) {
    // Тик на кадр, а не два, как у гоблина: хищник быстрее, и походка у него
    // должна быть быстрее — иначе бегущий выглядит крадущимся.
    constexpr std::uint64_t kTicksPerFrame = 1;
    constexpr std::uint64_t kCycle = kTicksPerFrame * kFrames;
    return static_cast<int>(((tick + id % kCycle) / kTicksPerFrame) % kFrames);
}

} // namespace WolfSprites

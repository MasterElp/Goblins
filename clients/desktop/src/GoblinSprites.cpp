#include "GoblinSprites.hpp"

#include <array>
#include <vector>

#include "SpriteAtlas.hpp"
#include "TileColors.hpp"

namespace GoblinSprites {

namespace {

// Имена кадров в файле рисунка. Порядок здесь — порядок, в котором их
// спрашивают отсюда же (frameAt ниже): возраст, занятие, кадр. В самом
// ресурсе он может быть любым — кадры ищутся по имени (Assets::frameIndex),
// и вот этот список тому единственная причина.
//
// Детёныш первым, а не взрослый: тем же порядком идут ступени и у дерева, и
// возвращает его stageOf. Список, идущий не в ту сторону, что число из
// stageOf, — самая тихая из возможных ошибок: рисунок остаётся рисунком,
// просто дети выглядят взрослыми.
constexpr std::array<const char*, kStages * kPoses * kFrames> kFrameNames = {
    "child.stand.a", "child.stand.b", "child.walk.a",  "child.walk.b",  "child.carry.a",
    "child.carry.b", "child.eat.a",   "child.eat.b",   "child.drink.a", "child.drink.b",
    "child.rest.a",  "child.rest.b",  "child.work.a",  "child.work.b",

    "adult.stand.a", "adult.stand.b", "adult.walk.a",  "adult.walk.b",  "adult.carry.a",
    "adult.carry.b", "adult.eat.a",   "adult.eat.b",   "adult.drink.a", "adult.drink.b",
    "adult.rest.a",  "adult.rest.b",  "adult.work.a",  "adult.work.b",
};

// Обводка. Тот же цвет, которым ромб обводился до всякого рисунка (см.
// WorldScreen): гоблин не должен потеряться на пёстрой земле, а цвета племён
// холодные и неяркие.
constexpr Color kOutline{20, 34, 32, 255};
// Глаз — единственное светлое пятно на всей фигуре, и это не украшение.
// Кожа у племён холодная и неяркая, обводка тёмная; тёмный глаз на такой
// голове сливался бы с обводкой в зарубку, а зарубка ничего не говорит.
// Светлый же виден издали — и виден он ровно с той стороны, в которую
// гоблин повёрнут, то есть сам по себе отвечает, куда тот идёт.
constexpr Color kEye{232, 202, 110, 255};
// Повязка — одна на все племена, и это не упущение, а тот же закон, по
// которому одинаковы навесы (BuildSprites): собственности в этом мире нет ни
// в одном законе, и племя опознаётся кожей, а не одеждой.
constexpr Color kCloth{86, 66, 48, 255};
// Ноша — солома и ветки. Того же цвета, что и куча материала на площадке
// (BuildSprites): это она и есть, только в руках.
constexpr Color kLoad{186, 168, 110, 255};
// Еда в горсти — ягодно-красная, как и сами ягоды на карте: несут не "нечто",
// а то самое, что рвали с куста.
constexpr Color kFood{178, 64, 62, 255};

Color darken(Color color, float amount) {
    return Color{static_cast<unsigned char>(color.r * (1.0f - amount)),
                 static_cast<unsigned char>(color.g * (1.0f - amount)),
                 static_cast<unsigned char>(color.b * (1.0f - amount)), color.a};
}

// Раскраска на племя. Меняется в ней одна кожа — всё прочее (обводка, глаз,
// повязка, ноша, еда) у всех племён общее, и по той же причине, по которой у
// деревьев общая кора: цвету оставлена одна работа — сказать, чьё это племя.
SpriteAtlas::Palette paletteOf(int tribe) {
    const Color skin = TileColors::goblinTribe(tribe);
    return {SpriteAtlas::Ink{'S', skin},     SpriteAtlas::Ink{'s', darken(skin, 0.30f)},
            SpriteAtlas::Ink{'o', kOutline}, SpriteAtlas::Ink{'E', kEye},
            SpriteAtlas::Ink{'L', kCloth},   SpriteAtlas::Ink{'M', kLoad},
            SpriteAtlas::Ink{'F', kFood}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не хватает
// хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): гоблин, лежащий
// вместо того, чтобы работать, хуже ромба, потому что выглядит как ответ на
// вопрос, которого никто не задавал.
const SpriteAtlas::Baked& baked() {
    static const SpriteAtlas::Baked result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(TileColors::kGoblinTribeCount);
        for (int tribe = 0; tribe < TileColors::kGoblinTribeCount; ++tribe) {
            palettes.push_back(paletteOf(tribe));
        }
        return SpriteAtlas::bake("goblin", palettes, kFrameNames);
    }();
    return result;
}

// Место кадра в списке имён. Номера вне пределов заворачиваются остатком, а
// не падают: возраст с номером больше, чем ступеней, — вопрос к тому, кто
// его посчитал, а не повод уронить клиент (то же правило, что и в
// SpriteAtlas::Sheet::source).
int frameAt(Pose pose, int stage, int frame) {
    const int p = static_cast<int>(pose) < 0 ? 0 : static_cast<int>(pose) % kPoses;
    const int s = stage < 0 ? 0 : stage % kStages;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return (s * kPoses + p) * kFrames + f;
}

} // namespace

bool ready() {
    return baked().complete;
}

const Texture2D& atlas() {
    return baked().sheet.texture();
}

Rectangle source(int tribe, Pose pose, int stage, int frame, int facing) {
    Rectangle piece = baked().source(tribe, frameAt(pose, stage, frame));
    if (facing < 0) {
        // Отрицательная ширина — то, как raylib просят отразить кусок по
        // горизонтали. Рисунок нарисован смотрящим вправо, второго набора
        // кадров на левую сторону нет вовсе (см. goblin.spr).
        piece.width = -piece.width;
    }
    return piece;
}

int stageOf(float growth) {
    // 0.90 — kBreedingGrowth из GoblinSystem, порог взрослости мира. Число
    // повторено, а не вынесено: клиент не подключает ядро вовсе
    // (07_TechStack.md, п.6), и приезжает к нему не порог, а развитость.
    return growth < 0.90f ? 0 : 1;
}

Pose poseOf(const std::string& desire, bool walking, bool loaded) {
    if (walking) {
        // Ноша важнее того, зачем он идёт: полные руки видно со стороны, а
        // намерение — нет. Она же и единственный ответ на вопрос "почему он
        // прошёл мимо куста".
        return loaded ? Pose::Carry : Pose::Walk;
    }
    // Дальше он стоит — и вот тут желание уже говорит, что именно он делает
    // на месте, потому что пришёл он туда ради этого.
    if (desire == "food") {
        return Pose::Eat;
    }
    if (desire == "water") {
        return Pose::Drink;
    }
    if (desire == "rest") {
        return Pose::Rest;
    }
    // Стройка и запас — оба про работу руками у земли: один вбивает жердь,
    // другой рвёт ягоды или складывает принесённое в кучу. Разными кадрами
    // они не различаются намеренно — со стороны это одно и то же движение, а
    // чем именно занят этот, скажет площадка или куча под ногами.
    if (desire == "build" || desire == "haul") {
        return Pose::Work;
    }
    // Сюда попадают "idle", "mate" и всякое желание, которого в мире ещё не
    // было, когда рисовались кадры. Стояние — честный ответ на все три:
    // ничем другим они со стороны и не выглядят.
    return Pose::Stand;
}

bool walkingNow(std::uint64_t stepTick, std::uint64_t tick) {
    // Четыре тика — самый долгий шаг в мире (скорость от 300 тысячных
    // клетки за тик). Столько же и помним: меньше — идущий мигал бы,
    // заметно больше — остановившийся ещё несколько тиков "шёл" бы на месте.
    constexpr std::uint64_t kWalkMemory = 4;
    // Ноль — шага не видели ни разу: гоблин только что родился или мир
    // только что открыт. Стоит, а не идёт: первый же его шаг это исправит.
    if (stepTick == 0 || tick < stepTick) {
        return false;
    }
    return tick - stepTick <= kWalkMemory;
}

int frameOf(std::uint64_t id, std::uint64_t tick) {
    // Сколько тиков держится один кадр. Два: при обычном тике в пятую долю
    // секунды это те же полсекунды, за которые качается дерево, — быстрее
    // гоблин дрожит, медленнее движение перестаёт читаться.
    constexpr std::uint64_t kTicksPerFrame = 2;
    constexpr std::uint64_t kCycle = kTicksPerFrame * kFrames;
    return static_cast<int>(((tick + id % kCycle) / kTicksPerFrame) % kFrames);
}

} // namespace GoblinSprites

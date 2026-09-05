#include "WaterSprites.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "SpriteAtlas.hpp"
#include "TileColors.hpp"

namespace WaterSprites {

namespace {

// Виды ряби. Порядок — тот же, в котором они перечислены в kFrameNames.
enum class Kind { Calm = 0, Side, Down, Diag };
constexpr int kKinds = 4;

// Имена кадров в файле рисунка. Порядок здесь — порядок, в котором их
// спрашивают отсюда же (frameAt ниже): вид, разводье, кадр. В самом ресурсе
// он может быть любым — кадры ищутся по имени (Assets::frameIndex), и вот
// этот список тому единственная причина.
constexpr std::array<const char*, kKinds * kVariants * kFrames> kFrameNames = {
    "calm.0.a", "calm.0.b", "calm.0.c", "calm.0.d", "calm.1.a", "calm.1.b",
    "calm.1.c", "calm.1.d", "calm.2.a", "calm.2.b", "calm.2.c", "calm.2.d",

    "side.0.a", "side.0.b", "side.0.c", "side.0.d", "side.1.a", "side.1.b",
    "side.1.c", "side.1.d", "side.2.a", "side.2.b", "side.2.c", "side.2.d",

    "down.0.a", "down.0.b", "down.0.c", "down.0.d", "down.1.a", "down.1.b",
    "down.1.c", "down.1.d", "down.2.a", "down.2.b", "down.2.c", "down.2.d",

    "diag.0.a", "diag.0.b", "diag.0.c", "diag.0.d", "diag.1.a", "diag.1.b",
    "diag.1.c", "diag.1.d", "diag.2.a", "diag.2.b", "diag.2.c", "diag.2.d",
};

// Глубина, по которой берётся цвет ступени. Границы — трети той же полной
// глубины, на которой строит свой переход сам цвет воды (TileColors::water
// делит на 3.0): рябь обязана лежать в тон клетке, на которой лежит, иначе
// на границе ступеней вода расслаивается полосами.
constexpr std::array<float, kDepths> kDepthSample = {0.5f, 1.5f, 3.0f};
constexpr float kDepthStepAt = 1.0f;

// Ниже этого перепада поверхности вода считается стоячей.
//
// Порог стои́т на шуме округления, а не на физике, и это здесь главное. Высота
// и глубина приезжают сотыми долями (shared/protocol/WirePrecision.hpp), то
// есть каждая с ошибкой до половины сотой; поверхность — их сумма, разность
// двух поверхностей — вчетверо больше, до двух сотых. Перепад мельче этого не
// направление, а рябь самого округления, и показывать по нему сторону течения
// значило бы рисовать стрелку на подброшенной монете.
//
// Пять сотых — вдвое с лишним выше этого шума. Замерено
// (tools/measure_water_flow.py): на живом мире это ровно медиана — половина
// водяных клеток оказывается стоячей, и это пруды с ровными плёсами, а
// текущей — другая половина, и это русла.
constexpr float kStillDrop = 0.05f;

// Перепад, быстрее которого рябь уже не ускоряется.
//
// Замерено на живом мире (tools/measure_water_flow.py): перепад поверхности
// на водяной клетке — 0.05 у половины из них, 0.16 у трёх четвертей, 0.29 у
// девяти десятых, 0.40 у девятнадцати из двадцати. Половина единицы — выше
// девяносто пятого процентиля: быстрее неё течёт только то, что льётся с
// уступа, и разгонять рябь дальше уже некуда.
constexpr float kFastDrop = 0.5f;

// Тиков на кадр: у самой медленной струи, у самой быстрой и у стоячей воды.
// Стоячая меняется медленнее самой медленной струи — пруд не бежит, он
// шевелится.
constexpr float kSlowTicks = 4.0f;
constexpr float kFastTicks = 1.0f;
constexpr std::uint64_t kCalmTicks = 7;

// Соседи по кругу, начиная с левого верхнего.
constexpr std::array<int, 8> kDx = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr std::array<int, 8> kDy = {-1, -1, -1, 0, 0, 1, 1, 1};

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

Color faded(Color color, unsigned char alpha) {
    return Color{color.r, color.g, color.b, alpha};
}

// Насколько заметна рябь на каждой ступени глубины. Чем глубже, тем слабее, и
// это не украшение: у мелкой воды под поверхностью светлое дно, и всякая
// складка на ней бликует, а у глубокой под поверхностью темнота — блику
// взяться неоткуда. Ровная по всем глубинам рябь превращала глубину в
// заштрихованное поле, на котором не видно ни глубины, ни того, что по этой
// воде плывёт.
constexpr std::array<unsigned char, kDepths> kCrestAlpha = {132, 104, 74};

// Раскраска на ступень глубины. Все четыре краски полупрозрачны, и это здесь
// несущее: под рябью лежит цвет клетки, посчитанный по ЕЁ глубине, а не по
// ступени, — сквозь прозрачную рябь он и виден, поэтому три ступени не
// разваливают воду на три полосы.
SpriteAtlas::Palette paletteOf(int step) {
    const std::size_t i = static_cast<std::size_t>(step);
    const Color base = TileColors::water(kDepthSample[i]);
    const unsigned char crest = kCrestAlpha[i];
    const auto part = [](unsigned char alpha, float share) {
        return static_cast<unsigned char>(alpha * share);
    };
    return {SpriteAtlas::Ink{'H', faded(lighten(base, 0.45f), crest)},
            SpriteAtlas::Ink{'h', faded(lighten(base, 0.26f), part(crest, 0.72f))},
            SpriteAtlas::Ink{'S', faded(darken(base, 0.22f), part(crest, 0.62f))},
            SpriteAtlas::Ink{'s', faded(darken(base, 0.40f), part(crest, 0.85f))}};
}

// Печётся при первом обращении: нужен уже созданный GL-контекст. Не хватает
// хоть одного кадра — не рисуем ничем (SpriteAtlas::bake): вода, стоящая
// вместо того, чтобы течь, хуже воды без ряби вовсе, потому что выглядит как
// ответ, а отвечает неверно.
const SpriteAtlas::Detailed& baked() {
    static const SpriteAtlas::Detailed result = [] {
        std::vector<SpriteAtlas::Palette> palettes;
        palettes.reserve(kDepths);
        for (int step = 0; step < kDepths; ++step) {
            palettes.push_back(paletteOf(step));
        }
        return SpriteAtlas::bakeDetailed("water", palettes, kFrameNames);
    }();
    return result;
}

// Номера вне пределов заворачиваются остатком, а не падают (то же правило,
// что и в SpriteAtlas::Placement::source).
int frameAt(Kind kind, int variant, int frame) {
    const int k = static_cast<int>(kind) < 0 ? 0 : static_cast<int>(kind) % kKinds;
    const int v = variant < 0 ? 0 : variant % kVariants;
    const int f = frame < 0 ? 0 : frame % kFrames;
    return (k * kVariants + v) * kFrames + f;
}

} // namespace

Flow flowAt(const std::vector<float>& terrainHeight, const std::vector<float>& depth, int width,
            int areaHeight, int x, int y) {
    Flow flow;
    if (width <= 0 || x < 0 || y < 0 || x >= width || y >= areaHeight) {
        return flow;
    }
    const std::size_t here = static_cast<std::size_t>(y) * width + x;
    if (here >= depth.size() || here >= terrainHeight.size()) {
        return flow;
    }

    const float surface = terrainHeight[here] + depth[here];
    // Корень из двух: диагональный сосед дальше прямого, и перепад до него
    // сравнивается с прямым только поделённым на расстояние. Без деления
    // всякая река текла бы наискосок — по диагонали перепад больше просто
    // потому, что клетка дальше.
    constexpr float kDiagonal = 1.41421356f;

    float steepest = 0.0f;
    for (int dir = 0; dir < 8; ++dir) {
        const int nx = x + kDx[static_cast<std::size_t>(dir)];
        const int ny = y + kDy[static_cast<std::size_t>(dir)];
        if (nx < 0 || ny < 0 || nx >= width || ny >= areaHeight) {
            continue;
        }
        const std::size_t there = static_cast<std::size_t>(ny) * width + nx;
        if (there >= depth.size() || there >= terrainHeight.size()) {
            continue;
        }
        const float step = kDx[static_cast<std::size_t>(dir)] != 0 &&
                                   kDy[static_cast<std::size_t>(dir)] != 0
                               ? kDiagonal
                               : 1.0f;
        const float drop = (surface - (terrainHeight[there] + depth[there])) / step;
        if (drop > steepest) {
            steepest = drop;
            flow.dx = kDx[static_cast<std::size_t>(dir)];
            flow.dy = kDy[static_cast<std::size_t>(dir)];
        }
    }

    if (steepest <= kStillDrop) {
        // Сторону тоже гасим: рисунок выбирается по dx/dy, и течение без
        // скорости показало бы бегущую струю у стоячего пруда.
        return Flow{};
    }
    flow.speed = std::min(1.0f, (steepest - kStillDrop) / (kFastDrop - kStillDrop));
    return flow;
}

Detail detailFor(float tileSize) {
    return baked().detailFor(tileSize);
}

bool ready(Detail detail) {
    return baked().ready(detail);
}

const Texture2D& atlas(Detail detail) {
    return baked().texture(detail);
}

int depthStep(float depth) {
    if (depth < kDepthStepAt) {
        return 0;
    }
    if (depth < kDepthStepAt * 2.0f) {
        return 1;
    }
    return 2;
}

int variantOf(int x, int y) {
    // Множители не те же, что у травы (3, 7) и у крон (5, 3), и это не
    // придирка: по остатку от трёх травяные дают полосы поперёк, древесные —
    // вдоль, а эти наискось. Совпади они, трава на берегу и вода у берега
    // разводились бы одним и тем же узором, и река с лугом читались бы одной
    // поверхностью.
    return ((x * 7 + y * 11) % kVariants + kVariants) % kVariants;
}

int frameOf(const Flow& flow, std::uint64_t tick) {
    std::uint64_t ticks = kCalmTicks;
    if (flow.speed > 0.0f) {
        const float scaled = kSlowTicks - (kSlowTicks - kFastTicks) * std::clamp(flow.speed, 0.0f, 1.0f);
        ticks = static_cast<std::uint64_t>(std::max(1.0f, std::round(scaled)));
    }
    // Фазы от клетки тут нет, в отличие от травы и деревьев, и это нарочно:
    // ветер треплет каждый куст сам по себе, а река движется телом. Сбей
    // соседние клетки по фазе — и течение рассыплется в мельтешение.
    return static_cast<int>((tick / ticks) % kFrames);
}

Rectangle source(Detail detail, int depth, const Flow& flow, int variant, int frame) {
    Kind kind = Kind::Calm;
    int flipX = 1;
    int flipY = 1;
    if (flow.speed > 0.0f && (flow.dx != 0 || flow.dy != 0)) {
        if (flow.dy == 0) {
            kind = Kind::Side;
            flipX = flow.dx;
        } else if (flow.dx == 0) {
            kind = Kind::Down;
            flipY = flow.dy;
        } else {
            kind = Kind::Diag;
            flipX = flow.dx;
            flipY = flow.dy;
        }
    }
    const int step = depth < 0 ? 0 : depth % kDepths;
    Rectangle piece = baked().sheet(detail).source(step, frameAt(kind, variant, frame));
    // Отрицательные ширина и высота — то, как raylib просят отразить кусок
    // (DrawTexturePro, rtextures.c). Узор смыкается сам с собой по обеим осям,
    // поэтому отражение не сдвигает его внутри клетки и не рвёт шов с
    // соседней.
    if (flipX < 0) {
        piece.width = -piece.width;
    }
    if (flipY < 0) {
        piece.height = -piece.height;
    }
    return piece;
}

} // namespace WaterSprites

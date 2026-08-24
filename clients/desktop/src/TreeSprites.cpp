#include "TreeSprites.hpp"

#include <cmath>
#include <vector>

#include "TileColors.hpp"

namespace TreeSprites {

namespace {

// Рисунок кадра: kHeight строк по kWidth знаков, сверху вниз. Верхняя
// половина (шестнадцать строк) — клетка НАД деревом, нижняя — его
// собственная.
//
// Знаки:
//   '.' — пусто, сквозь него видно землю;
//   'C' — крона, цвет вида;
//   'c' — тень в кроне, тот же цвет потемнее;
//   'T' — освещённая сторона ствола;
//   't' — ствол и корни.
//
// Второй кадр каждого возраста — не сдвиг первого целиком: качается крона,
// а комель стоит. Поэтому кадры нарисованы порознь, и у второго верх кроны
// уведён вправо, а низ остался на месте — так ветка кланяется, а не дерево
// прыгает.

// --- Росток ---
// Только своя клетка, и та наполовину пуста: два листа на стебле. Ствола
// как такового ещё нет, корней не видно вовсе.
constexpr const char* kSprout[2][kHeight] = {
    {
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "......cCc.......", ".....CCCCC......", "....CCcCCCC.....",
        ".....CCCCC......", "......CtC.......", ".......t........", "......ttt.......",
    },
    {
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", ".......cCc......", "......CCCCC.....", ".....CCcCCCC....",
        "......CCCCC.....", "......CtC.......", ".......t........", "......ttt.......",
    },
};

// --- Подрост ---
// Крона поднялась в клетку выше, но заняла только её низ; ствол уже
// настоящий, с освещённой стороной, корни едва намечены.
constexpr const char* kYoung[2][kHeight] = {
    {
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", ".......CC.......",
        ".....CCCCCC.....", "....CCCCcCCC....", "...CCCcCCCCCC...", "...CCCCCCCcCC...",
        "....CCCCCCCC....", ".....CCCCCC.....", "......cCCc......", ".......tT.......",
        ".......tT.......", "......ttT.......", ".......tT.......", ".......tTt......",
        ".......tT.......", ".......tT.......", "......ttT.......", ".......tT.......",
        ".......tT.......", ".......tT.......", "......ttTt......", ".....tt..tt.....",
    },
    {
        "................", "................", "................", "................",
        "................", "................", "................", "................",
        "................", "................", "................", "........CC......",
        "......CCCCCC....", ".....CCCCcCCC...", "....CCCcCCCCCC..", "...CCCCCCCcCC...",
        "....CCCCCCCC....", ".....CCCCCC.....", "......cCCc......", ".......tT.......",
        ".......tT.......", "......ttT.......", ".......tT.......", ".......tTt......",
        ".......tT.......", ".......tT.......", "......ttT.......", ".......tT.......",
        ".......tT.......", ".......tT.......", "......ttTt......", ".....tt..tt.....",
    },
};

// --- Взрослое ---
// Крона заняла верхнюю клетку целиком, ствол проходит свою насквозь, корни
// разошлись в стороны.
constexpr const char* kMature[2][kHeight] = {
    {
        "......CCCC......", ".....CCCCCC.....", "...CCCCCCCCCC...", "..CCCCcCCCCCC...",
        "..CCCCCCCCcCCC..", ".CCcCCCCCCCCCC..", ".CCCCCCCcCCCCCC.", ".CCCCcCCCCCCCCC.",
        ".CCCCCCCCCCcCCC.", "..CCCCCcCCCCCC..", "..CCCCCCCCCCCC..", "..CCCCCCCCCCC...",
        "...CCCCCCCCCC...", "....CCCCCCCC....", ".....CCCCCC.....", "......cCCc......",
        ".......tT.......", ".......tT.......", ".......tT.......", "......ttT.......",
        ".......tTt......", ".......tT.......", ".......tT.......", "......ttTt......",
        ".......tT.......", ".......tT.......", ".......tT.......", "......ttT.......",
        ".....t.tT.t.....", "....tt.tT.tt....", "...tt..tT..tt...", "..tt...tT...tt..",
    },
    {
        ".......CCCC.....", "......CCCCCC....", "....CCCCCCCCCC..", "...CCCCcCCCCCC..",
        "...CCCCCCCCcCCC.", "..CCcCCCCCCCCCC.", "..CCCCCCCcCCCCCC", "..CCCCcCCCCCCCCC",
        ".CCCCCCCCCCcCCC.", "..CCCCCcCCCCCC..", "..CCCCCCCCCCCC..", "..CCCCCCCCCCC...",
        "...CCCCCCCCCC...", "....CCCCCCCC....", ".....CCCCCC.....", "......cCCc......",
        ".......tT.......", ".......tT.......", ".......tT.......", "......ttT.......",
        ".......tTt......", ".......tT.......", ".......tT.......", "......ttTt......",
        ".......tT.......", ".......tT.......", ".......tT.......", "......ttT.......",
        ".....t.tT.t.....", "....tt.tT.tt....", "...tt..tT..tt...", "..tt...tT...tt..",
    },
};

const char* const* frameArt(int stage, int frame) {
    switch (stage) {
        case 0: return kSprout[frame];
        case 1: return kYoung[frame];
        default: break;
    }
    return kMature[frame];
}

// Кора у всех видов одна: ствол — это ствол, а вид опознаётся кроной.
// Разводить его по видам значило бы отнять у кроны единственную работу,
// которую она делает.
constexpr Color kBark{58, 44, 32, 255};
constexpr Color kBarkLit{78, 60, 42, 255};

// Развитость, по которой берётся цвет кроны для этого возраста. Не
// середина ступени, а её верх: дерево красится в цвет того, чем оно уже
// стало, а не того, чем становится.
constexpr float kStageGrowth[kStages] = {0.2f, 0.6f, 1.0f};

Color darken(Color color, float amount) {
    return Color{static_cast<unsigned char>(color.r * (1.0f - amount)),
                 static_cast<unsigned char>(color.g * (1.0f - amount)),
                 static_cast<unsigned char>(color.b * (1.0f - amount)), color.a};
}

Texture2D buildAtlas() {
    // Строка атласа — вид, столбец — возраст и кадр. Все виды сразу, а не
    // текстура на вид: их не больше пяти, вместе они весят меньше экрана, а
    // одна текстура — это одна привязка на всю рощу.
    const int columns = kStages * kFrames;
    const int rows = TileColors::kTreeSpeciesCount;
    const int atlasWidth = columns * kWidth;
    const int atlasHeight = rows * kHeight;

    std::vector<Color> pixels(static_cast<std::size_t>(atlasWidth) * atlasHeight, Color{0, 0, 0, 0});

    for (int species = 0; species < rows; ++species) {
        for (int stage = 0; stage < kStages; ++stage) {
            const Color crown = TileColors::tree(species, kStageGrowth[stage]);
            const Color crownShade = darken(crown, 0.30f);
            for (int frame = 0; frame < kFrames; ++frame) {
                const char* const* art = frameArt(stage, frame);
                const int originX = (stage * kFrames + frame) * kWidth;
                const int originY = species * kHeight;
                for (int y = 0; y < kHeight; ++y) {
                    const char* row = art[y];
                    for (int x = 0; x < kWidth; ++x) {
                        Color color{0, 0, 0, 0};
                        switch (row[x]) {
                            case 'C': color = crown; break;
                            case 'c': color = crownShade; break;
                            case 'T': color = kBarkLit; break;
                            case 't': color = kBark; break;
                            default: break;
                        }
                        pixels[static_cast<std::size_t>(originY + y) * atlasWidth + (originX + x)] = color;
                    }
                }
            }
        }
    }

    Image image{};
    image.data = pixels.data();
    image.width = atlasWidth;
    image.height = atlasHeight;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    const Texture2D texture = LoadTextureFromImage(image);
    // Рисунок пиксельный: сглаживание превратило бы его в зелёное пятно на
    // любом масштабе, кроме единичного.
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    return texture;
}

} // namespace

const Texture2D& atlas() {
    static Texture2D texture = buildAtlas();
    return texture;
}

Rectangle source(int species, int stage, int frame) {
    const int row = species < 0 ? 0 : species % TileColors::kTreeSpeciesCount;
    const int column = (stage < 0 ? 0 : stage % kStages) * kFrames + (frame < 0 ? 0 : frame % kFrames);
    return Rectangle{static_cast<float>(column * kWidth), static_cast<float>(row * kHeight),
                     static_cast<float>(kWidth), static_cast<float>(kHeight)};
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

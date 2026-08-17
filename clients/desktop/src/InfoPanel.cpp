#include "InfoPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <raylib.h>

#include "TileColors.hpp"

namespace InfoPanel {

namespace {

constexpr int kTitleFont = 16;
constexpr int kFont = 14;
constexpr int kLineHeight = 18;
constexpr int kGroupGap = 8;
constexpr float kMinColumnWidth = 190.0f;

const Color kTitleColor{235, 200, 110, 255};
const Color kGroupColor{150, 210, 255, 255};
const Color kNameColor{195, 195, 200, 255};
const Color kValueColor{245, 245, 245, 255};
const Color kMutedColor{135, 135, 142, 255};

// Значения тут разного порядка — от 0.0002 (расход воды за тик) до 9400
// (предельный возраст). Один общий формат либо съел бы малые в ноль, либо
// растянул большие на десяток знаков, поэтому формат выбирается по самому
// числу — тем же способом, что и в оверлее констант.
std::string formatValue(float value) {
    const float magnitude = std::fabs(value);
    if (value == std::floor(value) && magnitude < 1e7f) {
        return TextFormat("%.0f", value);
    }
    if (magnitude < 0.01f) {
        return TextFormat("%.4f", value);
    }
    if (magnitude < 1.0f) {
        return TextFormat("%.3f", value);
    }
    return TextFormat("%.2f", value);
}

// Печать в колонку: строки идут сверху вниз, а когда упираются в нижний
// край панели — начинается следующая колонка. Панель узкая и высокая, а
// строк у взрослого животного под сорок (тело, желания, геном, клетка под
// ним), и обрывать их на середине значило бы прятать как раз то, ради чего
// карточку открывали.
class ColumnWriter {
public:
    ColumnWriter(Rectangle bounds, float columnWidth)
        : bounds_(bounds), columnWidth_(columnWidth), x_(bounds.x), y_(bounds.y) {}

    bool exhausted() const { return x_ + columnWidth_ > bounds_.x + bounds_.width + 1.0f; }

    void group(const std::string& title) {
        if (exhausted()) {
            return;
        }
        if (y_ > bounds_.y) {
            y_ += kGroupGap;
            wrapIfNeeded();
        }
        if (exhausted()) {
            return;
        }
        DrawText(title.c_str(), static_cast<int>(x_), static_cast<int>(y_), kFont, kGroupColor);
        advance();
    }

    void line(const std::string& name, const std::string& value, Color valueColor = kValueColor) {
        if (exhausted()) {
            return;
        }
        DrawText(name.c_str(), static_cast<int>(x_) + 6, static_cast<int>(y_), kFont, kNameColor);
        const int width = MeasureText(value.c_str(), kFont);
        DrawText(value.c_str(), static_cast<int>(x_ + columnWidth_) - width - 10, static_cast<int>(y_), kFont,
                 valueColor);
        advance();
    }

    void note(const std::string& text, Color color = kMutedColor) {
        if (exhausted()) {
            return;
        }
        DrawText(text.c_str(), static_cast<int>(x_), static_cast<int>(y_), kFont, color);
        advance();
    }

private:
    void advance() {
        y_ += kLineHeight;
        wrapIfNeeded();
    }

    void wrapIfNeeded() {
        if (y_ + kLineHeight <= bounds_.y + bounds_.height) {
            return;
        }
        x_ += columnWidth_;
        y_ = bounds_.y;
    }

    Rectangle bounds_;
    float columnWidth_;
    float x_;
    float y_;
};

const WorldState::Animal* findAnimal(const WorldState& state, std::uint64_t id) {
    for (const auto& animal : state.animals) {
        if (animal.id == id) {
            return &animal;
        }
    }
    return nullptr;
}

// Карточка сервера относится именно к этой цели? Между кликом и ответом
// проходит одна рассылка, и всё это время в state.watched лежит карточка
// предыдущего выбранного — показать её как текущую значило бы соврать.
bool watchedMatches(const WorldState& state, const Target& target) {
    if (target.kind == Target::Kind::Animal) {
        return state.watched.kind == "animal" && state.watched.id == target.animalId;
    }
    if (target.kind == Target::Kind::Plant) {
        return state.watched.kind == "plant" && state.watched.x == target.x && state.watched.y == target.y;
    }
    return false;
}

// Клетка под курсором (или под выбранным существом) — из общего снимка,
// без запроса к серверу: всё это в нём уже есть.
void drawTileGroup(const WorldState& state, ColumnWriter& writer, int x, int y) {
    if (x < 0 || y < 0 || x >= state.areaWidth || y >= state.areaHeight) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(y) * state.areaWidth + x;
    if (index >= state.moisture.size()) {
        return;
    }

    writer.group(TextFormat("Tile (%d,%d)", x, y));
    writer.line("moisture", formatValue(state.moisture[index]));
    writer.line("rockiness", formatValue(state.rockiness[index]));
    writer.line("compaction", formatValue(state.compaction[index]));
    writer.line("minerals", formatValue(static_cast<float>(state.minerals[index])));
    writer.line("height", formatValue(state.height[index]));
    if (state.waterDepth[index] > 0.0f) {
        writer.line("water", formatValue(state.waterDepth[index]));
    }
    if (state.humus[index] > 0) {
        writer.line("humus", formatValue(static_cast<float>(state.humus[index])));
    }
    if (index < state.carcass.size() && state.carcass[index] > 0.0f) {
        writer.line("carcass", formatValue(state.carcass[index]));
    }
    if (index < state.seedSpeciesAt.size() && state.seedSpeciesAt[index] >= 0) {
        writer.line("seed", TextFormat("sp%d", state.seedSpeciesAt[index]));
    }
    if (state.plantSpeciesAt[index] >= 0) {
        writer.line("grass", TextFormat("sp%d  %.0f%%", state.plantSpeciesAt[index], state.plantGrowth[index] * 100.0f));
    }
    // Сколько ещё зверей стоит на этой клетке — по ним и щёлкают, перебирая
    // выбор: без этого числа непонятно, почему клик по той же точке
    // показывает то одного, то другого.
    int animalsHere = 0;
    for (const auto& animal : state.animals) {
        if (animal.x == x && animal.y == y) {
            ++animalsHere;
        }
    }
    if (animalsHere > 0) {
        writer.line("animals here", TextFormat("%d", animalsHere));
    }
}

} // namespace

void draw(const WorldState& state, const Target& target, Rectangle bounds) {
    if (target.kind == Target::Kind::None) {
        DrawText("Hover the map to inspect a tile;", static_cast<int>(bounds.x), static_cast<int>(bounds.y), kFont,
                 kMutedColor);
        DrawText("click to follow a creature or a plant.", static_cast<int>(bounds.x),
                 static_cast<int>(bounds.y) + kLineHeight, kFont, kMutedColor);
        return;
    }

    // Заголовок: чем является выбранное и где оно сейчас. Цветной квадрат —
    // тот же цвет, которым существо нарисовано на карте, чтобы карточку и
    // точку на карте можно было связать взглядом.
    const WorldState::Animal* animal =
        target.kind == Target::Kind::Animal ? findAnimal(state, target.animalId) : nullptr;

    std::string title;
    Color swatch{0, 0, 0, 0};
    int tileX = target.x;
    int tileY = target.y;

    switch (target.kind) {
        case Target::Kind::Animal:
            if (animal != nullptr) {
                title = TextFormat("%s sp%d", animal->predator ? "Predator" : "Herbivore", animal->species);
                swatch = animal->predator ? TileColors::predatorSpecies(animal->species)
                                          : TileColors::herbivoreSpecies(animal->species);
                tileX = animal->x;
                tileY = animal->y;
            } else {
                title = "Creature is gone";
            }
            break;
        case Target::Kind::Plant:
            if (state.areaWidth > 0 && target.x >= 0 && target.y >= 0 && target.x < state.areaWidth &&
                target.y < state.areaHeight &&
                state.plantSpeciesAt[static_cast<std::size_t>(target.y) * state.areaWidth + target.x] >= 0) {
                const int species = state.plantSpeciesAt[static_cast<std::size_t>(target.y) * state.areaWidth + target.x];
                title = TextFormat("Grass sp%d", species);
                swatch = TileColors::plantSpecies(species);
            } else {
                title = "Plant is gone";
            }
            break;
        case Target::Kind::Soil:
            title = "Soil";
            break;
        case Target::Kind::None:
            break;
    }

    float titleX = bounds.x;
    if (swatch.a > 0) {
        DrawRectangle(static_cast<int>(bounds.x), static_cast<int>(bounds.y) + 3, 12, 12, swatch);
        titleX += 18.0f;
    }
    DrawText(title.c_str(), static_cast<int>(titleX), static_cast<int>(bounds.y), kTitleFont, kTitleColor);

    const char* pinLabel = target.pinned ? "tracked (click again to cycle)" : "under cursor";
    DrawText(pinLabel, static_cast<int>(bounds.x + bounds.width) - MeasureText(pinLabel, kFont) - 2,
             static_cast<int>(bounds.y) + 2, kFont, target.pinned ? kTitleColor : kMutedColor);

    std::string subtitle = TextFormat("at (%d,%d)", tileX, tileY);
    if (animal != nullptr) {
        subtitle += TextFormat("   %s   -> %s", animal->sex.c_str(), animal->desire.c_str());
        subtitle += TextFormat("   grown %.0f%%   health %.0f%%", animal->growth * 100.0f, animal->health * 100.0f);
    }
    DrawText(subtitle.c_str(), static_cast<int>(bounds.x), static_cast<int>(bounds.y) + kTitleFont + 4, kFont,
             kMutedColor);

    const float contentTop = bounds.y + kTitleFont + kLineHeight + 8.0f;
    const Rectangle content{bounds.x, contentTop, bounds.width, bounds.height - (contentTop - bounds.y)};
    if (content.height < kLineHeight * 2.0f) {
        return;
    }

    // Две колонки, если панель достаточно широка: у взрослого животного
    // тело, желания и геном — под сорок строк, а панель редко бывает выше
    // тридцати.
    const int columns = content.width >= kMinColumnWidth * 2.0f ? 2 : 1;
    ColumnWriter writer(content, content.width / static_cast<float>(columns));

    drawTileGroup(state, writer, tileX, tileY);

    if (target.kind == Target::Kind::Soil) {
        return;
    }
    if (target.kind == Target::Kind::Animal && animal == nullptr) {
        writer.group("Creature");
        writer.note("no longer in the world", Color{230, 130, 120, 255});
        return;
    }

    if (state.watched.kind == "gone" || !watchedMatches(state, target)) {
        writer.group(target.kind == Target::Kind::Animal ? "Creature" : "Plant");
        // "gone" — сервер уже ответил, что выбранного нет; иначе карточка
        // просто ещё едет (одна рассылка, до snapshot_interval_ms).
        writer.note(state.watched.kind == "gone" ? "no longer in the world" : "asking the server...",
                    state.watched.kind == "gone" ? Color{230, 130, 120, 255} : kMutedColor);
        return;
    }

    for (const auto& group : state.watched.groups) {
        writer.group(group.title);
        for (const auto& [name, value] : group.values) {
            writer.line(name, formatValue(value));
        }
    }
}

} // namespace InfoPanel

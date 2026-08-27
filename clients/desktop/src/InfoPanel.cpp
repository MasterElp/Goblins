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
// Чем занят — своим цветом: эту строку читают первой, и теряться среди
// имён и чисел она не должна.
const Color kDoingColor{170, 220, 160, 255};

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

const WorldState::Goblin* findGoblin(const WorldState& state, std::uint64_t id) {
    for (const auto& goblin : state.goblins) {
        if (goblin.id == id) {
            return &goblin;
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
    if (target.kind == Target::Kind::Goblin) {
        return state.watched.kind == "goblin" && state.watched.id == target.animalId;
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
    if (index < state.trampled.size()) {
        writer.line("trampled", formatValue(state.trampled[index]));
    }
    if (index < state.store.size() && state.store[index] > 0) {
        writer.line("store", TextFormat("%d", state.store[index]));
    }
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
    // Дерево — своей строкой и со своей нумерацией видов ("tr", а не "sp"):
    // список видов у него отдельный, и спутать их было бы легко.
    if (index < state.bushSpeciesAt.size() && state.bushSpeciesAt[index] >= 0) {
        // Ягоды — рядом с самим кустом: без них строка говорила бы только,
        // что куст стоит, а вопрос к нему всегда один — есть ли что рвать.
        const int berries = index < state.berries.size() ? state.berries[index] : 0;
        writer.line("bush", TextFormat("bu%d  %.0f%%  berries %d", state.bushSpeciesAt[index],
                                        state.plantGrowth[index] * 100.0f, berries));
    }
    if (index < state.treeSpeciesAt.size() && state.treeSpeciesAt[index] >= 0) {
        writer.line("tree", TextFormat("tr%d  %.0f%%", state.treeSpeciesAt[index], state.plantGrowth[index] * 100.0f));
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

// Что на клетке ПОСТРОЕНО — своей группой, а не строчками среди свойств
// земли, рядом с влажностью и камнями. Постройка землёй не является: её
// сделали руками, она ветшает, и её подновляют — а вопрос к ней всегда
// особый, крепка ли она и много ли осталось.
//
// Прочность процентами, а не долей: "навес 62%" и есть ответ мира, который
// слов "готово" и "не готово" не знает вовсе (BuildingComponent) — половина
// навеса укрывает вполовину.
void drawBuildingGroup(const WorldState& state, ColumnWriter& writer, int x, int y) {
    if (x < 0 || y < 0 || x >= state.areaWidth || y >= state.areaHeight) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(y) * state.areaWidth + x;
    const bool canopy = index < state.canopy.size() && state.canopy[index] > 0.0f;
    const bool bedding = index < state.bedding.size() && state.bedding[index] > 0.0f;
    const bool site = index < state.site.size() && state.site[index] > 0;
    if (!canopy && !bedding && !site) {
        return;
    }

    writer.group("Building");
    if (canopy) {
        writer.line("canopy", TextFormat("%.0f%%", state.canopy[index] * 100.0f));
    }
    if (bedding) {
        writer.line("bedding", TextFormat("%.0f%%", state.bedding[index] * 100.0f));
    }
    if (site) {
        writer.line("planned", state.site[index] == 1 ? "canopy" : "bedding");
        // Материал на площадке — всегда, пока есть замысел, и нулём тоже.
        // Ноль значит "работать нечем", и это самое частое, из-за чего
        // стройка стоит; прячась при нуле, строка прятала бы ровно тот
        // случай, ради которого её и читают.
        const int material = index < state.siteMaterial.size() ? state.siteMaterial[index] : 0;
        writer.line("material", TextFormat("%d", material), material > 0 ? kValueColor : kMutedColor);
    }
}

// Что гоблин ПОМНИТ — списком мест, а не только кольцами на карте.
//
// Кольца отвечают "куда он пойдёт", список — "чем это место было ему
// хорошо и насколько твёрдо оно помнится". Второе кольцами не сказать:
// цвет различает четыре вида мест, а толщина — твёрдость, и читать по ней
// число нельзя. Между тем именно число объясняет, почему гоблин прошёл
// мимо ближнего ягодника к дальнему: у дальнего память крепче.
//
// Сортировка по твёрдости, а не в порядке ячеек памяти: голова у гоблина
// на восемь мест (core/Knowledge.hpp), место в ней ничего не значит, а
// первым читать надо то, что вернее позовёт.
void drawMemoryGroup(const WorldState& state, ColumnWriter& writer) {
    if (state.watched.knows.empty()) {
        return;
    }
    std::vector<const WorldState::Watched::Known*> places;
    places.reserve(state.watched.knows.size());
    for (const auto& place : state.watched.knows) {
        places.push_back(&place);
    }
    std::sort(places.begin(), places.end(),
              [](const WorldState::Watched::Known* a, const WorldState::Watched::Known* b) {
                  return a->strength > b->strength;
              });

    writer.group("Memory");
    for (const auto* place : places) {
        writer.line(place->kind, TextFormat("(%d,%d)  %d%%", place->x, place->y, place->strength));
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
    const WorldState::Goblin* goblin =
        target.kind == Target::Kind::Goblin ? findGoblin(state, target.animalId) : nullptr;

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
        case Target::Kind::Goblin:
            if (goblin != nullptr) {
                title = TextFormat("Goblin tribe %d", goblin->tribe);
                swatch = TileColors::goblinTribe(goblin->tribe);
                tileX = goblin->x;
                tileY = goblin->y;
            } else {
                title = "Goblin is gone";
            }
            break;
        case Target::Kind::Plant:
            if (state.areaWidth > 0 && target.x >= 0 && target.y >= 0 && target.x < state.areaWidth &&
                target.y < state.areaHeight) {
                const std::size_t cell = static_cast<std::size_t>(target.y) * state.areaWidth + target.x;
                // Где стоит дерево, травы нет: растение на клетке одно,
                // поэтому и заголовок берётся из того слоя, который занят.
                if (cell < state.treeSpeciesAt.size() && state.treeSpeciesAt[cell] >= 0) {
                    title = TextFormat("Tree tr%d", state.treeSpeciesAt[cell]);
                    swatch = TileColors::treeSpecies(state.treeSpeciesAt[cell]);
                } else if (cell < state.bushSpeciesAt.size() && state.bushSpeciesAt[cell] >= 0) {
                    title = TextFormat("Bush bu%d", state.bushSpeciesAt[cell]);
                    swatch = TileColors::bushSpecies(state.bushSpeciesAt[cell]);
                } else if (state.plantSpeciesAt[cell] >= 0) {
                    title = TextFormat("Grass sp%d", state.plantSpeciesAt[cell]);
                    swatch = TileColors::plantSpecies(state.plantSpeciesAt[cell]);
                } else {
                    title = "Plant is gone";
                }
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
    // Пол, желание, взрослость и целость — одинаково у зверя и у гоблина:
    // тело у них одно (core/Body.hpp), и разной строки оно не заслуживает.
    const auto appendBody = [&subtitle](const std::string& sex, const std::string& desire, float growth,
                                         float health) {
        subtitle += TextFormat("   %s   -> %s", sex.c_str(), desire.c_str());
        subtitle += TextFormat("   grown %.0f%%   health %.0f%%", growth * 100.0f, health * 100.0f);
    };
    if (animal != nullptr) {
        appendBody(animal->sex, animal->desire, animal->growth, animal->health);
    }
    if (goblin != nullptr) {
        appendBody(goblin->sex, goblin->desire, goblin->growth, goblin->health);
    }
    DrawText(subtitle.c_str(), static_cast<int>(bounds.x), static_cast<int>(bounds.y) + kTitleFont + 4, kFont,
             kMutedColor);

    float contentTop = bounds.y + kTitleFont + kLineHeight + 8.0f;

    // Чем занят — отдельной строкой во всю ширину панели, а не парой в
    // группе: это фраза, а колонка шириной в двадцать знаков её обрежет на
    // середине. Желание отвечает "чего он хочет", эта строка — "что он с
    // этим делает", и второе из первого не выводится: хотеть есть можно и
    // стоя над кустом, и за полкарты от него.
    //
    // Только когда карточка пришла именно про эту цель: между кликом и
    // ответом сервера в state.watched лежит ещё предыдущий выбранный, и
    // соврать этой строкой хуже всего — она короткая, заметная, и читают её
    // первой.
    if (!state.watched.doing.empty() && watchedMatches(state, target)) {
        DrawText(state.watched.doing.c_str(), static_cast<int>(bounds.x), static_cast<int>(contentTop), kFont,
                 kDoingColor);
        contentTop += kLineHeight;
    }
    const Rectangle content{bounds.x, contentTop, bounds.width, bounds.height - (contentTop - bounds.y)};
    if (content.height < kLineHeight * 2.0f) {
        return;
    }

    // Две колонки, если панель достаточно широка: у взрослого животного
    // тело, желания и геном — под сорок строк, а панель редко бывает выше
    // тридцати.
    const int columns = content.width >= kMinColumnWidth * 2.0f ? 2 : 1;
    ColumnWriter writer(content, content.width / static_cast<float>(columns));

    // Клетка — последней группой, а не первой: у неё то и дело появляются
    // и пропадают строки (вода, перегной, семя, падаль — все условные), и
    // будь она первой, каждое такое появление сдвигало бы вниз всё тело,
    // желания и геном под ним. Существу или траве, за которыми следят,
    // это мешает больше всего — их и разглядывают дольше всего. Клетка
    // после них может прыгать сама с собой сколько угодно: ниже нет
    // ничего, чему бы это помешало.
    if (target.kind != Target::Kind::Soil) {
        const bool creatureGone = (target.kind == Target::Kind::Animal && animal == nullptr) ||
                                   (target.kind == Target::Kind::Goblin && goblin == nullptr);
        if (creatureGone) {
            writer.group("Creature");
            writer.note("no longer in the world", Color{230, 130, 120, 255});
        } else if (state.watched.kind == "gone" || !watchedMatches(state, target)) {
            writer.group(target.kind == Target::Kind::Plant ? "Plant" : "Creature");
            // "gone" — сервер уже ответил, что выбранного нет; иначе карточка
            // просто ещё едет (одна рассылка, до snapshot_interval_ms).
            writer.note(state.watched.kind == "gone" ? "no longer in the world" : "asking the server...",
                        state.watched.kind == "gone" ? Color{230, 130, 120, 255} : kMutedColor);
        } else {
            for (const auto& group : state.watched.groups) {
                writer.group(group.title);
                for (const auto& [name, value] : group.values) {
                    writer.line(name, formatValue(value));
                }
            }
            // Память — последней из карточки существа и только у того, у
            // кого она есть: у зверя её нет вовсе, и пустой группой это
            // говорить незачем.
            drawMemoryGroup(state, writer);
        }
    }

    drawTileGroup(state, writer, tileX, tileY);
    drawBuildingGroup(state, writer, tileX, tileY);
}

} // namespace InfoPanel

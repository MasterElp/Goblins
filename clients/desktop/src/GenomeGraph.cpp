#include "GenomeGraph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <raylib.h>

namespace GenomeGraph {

namespace {

constexpr int kTitleFont = 14;
constexpr int kAxisFont = 12;

// Разметка панели одного графика. Шапка выше, чем у графика численности:
// под заголовком стоит легенда из имён черт, а их у травы полтора десятка,
// и в одну строку они не ложатся никак.
constexpr float kValueLabelWidth = 36.0f;
constexpr float kTitleHeight = 18.0f;
constexpr float kLegendLineHeight = 14.0f;
constexpr int kLegendLines = 3;
constexpr float kFooterHeight = 17.0f;
constexpr float kHeaderHeight = kTitleHeight + kLegendLineHeight * kLegendLines + 4.0f;

using History = std::vector<WorldState::PopulationPoint>;
using Traits = std::vector<WorldState::PopulationTrait>;

const Color kMutedColor{130, 130, 138, 255};
const Color kTitleColor{200, 200, 205, 255};
const Color kGridColor{52, 52, 60, 255};
const Color kCursorColor{255, 220, 90, 255};

enum class Kind { Plants, Trees, Herbivores, Predators, Goblins };

const std::vector<int>& genomeOf(const WorldState::PopulationPoint& point, Kind kind) {
    switch (kind) {
        case Kind::Plants: return point.plantGenome;
        case Kind::Trees: return point.treeGenome;
        case Kind::Predators: return point.predatorGenome;
        case Kind::Goblins: return point.goblinGenome;
        case Kind::Herbivores: break;
    }
    return point.herbivoreGenome;
}

// Цвет черты — по её номеру, кругом по цветовому кругу. Не из палитр
// TileColors: те намеренно тематические (зелень травы, охра стада), вид от
// вида в них отличается оттенком, — а тут рядом лежит полтора десятка
// линий, и различать их нужно наверняка. Шаг по тону взят не подряд, а
// через семь позиций из шестнадцати: соседние по таблице черты получают
// далёкие цвета, и пара, идущая рядом в легенде, не сливается на графике.
Color traitColor(std::size_t index) {
    constexpr int kWheel = 16;
    const float hue = static_cast<float>((index * 7) % kWheel) * (360.0f / kWheel);
    // Второй оборот круга — темнее: так различимы и черты, у которых тон
    // совпал (больше шестнадцати черт в таблице пока нет, но повтор тона
    // должен оставаться читаемым).
    const float value = (index / kWheel) % 2 == 0 ? 0.95f : 0.70f;
    return ColorFromHSV(hue, 0.62f, value);
}

// Вложение черты, 0..1: где значение гена стоит между худшим для существа
// (lo) и лучшим (hi). Направление зашито в саму пару — у возраста
// взросления lo это "поздно", а hi "рано", — поэтому делить нужно именно
// на (hi - lo), со знаком.
float investmentOf(const WorldState::PopulationTrait& trait, int value) {
    if (trait.hi == trait.lo) {
        // Черта без диапазона (в таблицах ядра такой нет, но данные
        // приходят извне): рисуем посередине, чтобы линия не легла в край
        // и не притворилась крайним вложением.
        return 0.5f;
    }
    const float t = static_cast<float>(value - trait.lo) / static_cast<float>(trait.hi - trait.lo);
    return std::clamp(t, 0.0f, 1.0f);
}

Rectangle plotOf(Rectangle area) {
    return Rectangle{area.x + kValueLabelWidth, area.y + kHeaderHeight,
                     std::max(1.0f, area.width - kValueLabelWidth - 6.0f),
                     std::max(1.0f, area.height - kHeaderHeight - kFooterHeight)};
}

// Ось времени — по тику, а не по номеру точки: при прореживании летописи
// старая половина идёт с одним шагом, а новая с другим (см.
// PopulationHistory::thin), и по номерам время шло бы неравномерно.
float xAt(const History& history, const Rectangle& plot, std::uint64_t tick) {
    const std::uint64_t first = history.front().tick;
    const std::uint64_t last = history.back().tick;
    const double span = last > first ? static_cast<double>(last - first) : 1.0;
    const double t = std::clamp(static_cast<double>(tick - first) / span, 0.0, 1.0);
    return plot.x + static_cast<float>(t) * plot.width;
}

float yAt(const Rectangle& plot, float investment) {
    return plot.y + plot.height - std::clamp(investment, 0.0f, 1.0f) * plot.height;
}

std::size_t nearestPoint(const History& history, const Rectangle& plot, float x) {
    const std::uint64_t first = history.front().tick;
    const std::uint64_t last = history.back().tick;
    const double span = last > first ? static_cast<double>(last - first) : 1.0;
    const double t = std::clamp((x - plot.x) / plot.width, 0.0f, 1.0f);
    const auto target = first + static_cast<std::uint64_t>(std::llround(t * span));

    const auto it = std::lower_bound(
        history.begin(), history.end(), target,
        [](const WorldState::PopulationPoint& point, std::uint64_t value) { return point.tick < value; });
    if (it == history.begin()) {
        return 0;
    }
    if (it == history.end()) {
        return history.size() - 1;
    }
    const auto previous = std::prev(it);
    const bool previousIsCloser = (target - previous->tick) <= (it->tick - target);
    return static_cast<std::size_t>((previousIsCloser ? previous : it) - history.begin());
}

// Один график: одна диета, по линии на черту. hasHover/hoverIndex — общая
// для всех панелей точка под курсором: сравнивать геном травы с
// геномом того, кто её ест, имеет смысл только в один и тот же момент.
void drawChart(const History& history, const Traits& traits, Rectangle area, Kind kind, const char* title,
               bool hasHover, std::size_t hoverIndex, const char* footerNote) {
    DrawText(title, static_cast<int>(area.x), static_cast<int>(area.y), kTitleFont, kTitleColor);

    const WorldState::PopulationPoint& shown = hasHover ? history[hoverIndex] : history.back();
    const char* tickLabel = hasHover ? TextFormat("tick %llu", static_cast<unsigned long long>(shown.tick))
                                      : TextFormat("tick %llu (now)", static_cast<unsigned long long>(shown.tick));
    DrawText(tickLabel, static_cast<int>(area.x + area.width) - MeasureText(tickLabel, kAxisFont),
             static_cast<int>(area.y) + 2, kAxisFont, hasHover ? kCursorColor : kMutedColor);

    const Rectangle plot = plotOf(area);
    if (plot.width < 40.0f || plot.height < 24.0f) {
        return;
    }

    if (traits.empty()) {
        DrawText("no traits from the server", static_cast<int>(plot.x) + 6, static_cast<int>(plot.y) + 6, kAxisFont,
                 kMutedColor);
        return;
    }
    // Последняя точка без генома — этой диеты в мире сейчас нет вовсе. То
    // же самое в середине летописи просто рвёт кривую, а тут сказать надо
    // словами: пустой график ничего не объясняет.
    if (genomeOf(history.back(), kind).empty()) {
        DrawText("none alive", static_cast<int>(plot.x) + 6, static_cast<int>(plot.y) + 6, kAxisFont, kMutedColor);
        return;
    }

    // Легенда: имя черты своим цветом и её среднее значение в показываемый
    // момент. Она же и есть readout под курсором — числа те же, просто из
    // другой точки. Имена длинные ("moisture_capacity"), поэтому строк
    // несколько, а лишнее обрывается: лучше не показать три черты из
    // пятнадцати, чем налезть на сам график.
    const auto& values = genomeOf(shown, kind);
    {
        float legendX = area.x;
        float legendY = area.y + kTitleHeight;
        int line = 0;
        for (std::size_t t = 0; t < traits.size(); ++t) {
            const char* label = TextFormat("%s %d", traits[t].name.c_str(), t < values.size() ? values[t] : 0);
            const float entryWidth = 11.0f + static_cast<float>(MeasureText(label, kAxisFont)) + 10.0f;
            if (legendX + entryWidth > area.x + area.width) {
                ++line;
                if (line >= kLegendLines) {
                    break;
                }
                legendX = area.x;
                legendY += kLegendLineHeight;
            }
            DrawRectangle(static_cast<int>(legendX), static_cast<int>(legendY) + 1, 8, 8, traitColor(t));
            DrawText(label, static_cast<int>(legendX) + 11, static_cast<int>(legendY), kAxisFont, kTitleColor);
            legendX += entryWidth;
        }
    }

    // Сетка — по вложению, а не по значению: 0 — черта отдана целиком,
    // 100 — вложена целиком. Сами значения генов стоят в легенде.
    for (int step = 0; step <= 2; ++step) {
        const float investment = static_cast<float>(step) * 0.5f;
        const int y = static_cast<int>(yAt(plot, investment));
        DrawLine(static_cast<int>(plot.x), y, static_cast<int>(plot.x + plot.width), y, kGridColor);
        const char* label = TextFormat("%d%%", step * 50);
        DrawText(label, static_cast<int>(plot.x) - 6 - MeasureText(label, kAxisFont), y - kAxisFont / 2, kAxisFont,
                 kMutedColor);
    }

    for (std::size_t t = 0; t < traits.size(); ++t) {
        const Color color = traitColor(t);
        Vector2 previous{};
        bool hasPrevious = false;
        for (const auto& point : history) {
            const auto& genome = genomeOf(point, kind);
            // Точки без генома этой диеты (её тогда не было в живых, либо
            // летопись велась до того, как геном стали записывать) рвут
            // кривую, а не тянут её через пустоту прямой.
            if (t >= genome.size()) {
                hasPrevious = false;
                continue;
            }
            const Vector2 screen{xAt(history, plot, point.tick), yAt(plot, investmentOf(traits[t], genome[t]))};
            if (hasPrevious) {
                DrawLineEx(previous, screen, 1.5f, color);
            }
            previous = screen;
            hasPrevious = true;
        }
    }

    if (hasHover) {
        const float x = xAt(history, plot, shown.tick);
        DrawLine(static_cast<int>(x), static_cast<int>(plot.y), static_cast<int>(x),
                 static_cast<int>(plot.y + plot.height), Color{255, 220, 90, 120});
        for (std::size_t t = 0; t < traits.size() && t < values.size(); ++t) {
            DrawCircle(static_cast<int>(x), static_cast<int>(yAt(plot, investmentOf(traits[t], values[t]))), 2.5f,
                       traitColor(t));
        }
    }

    const int axisY = static_cast<int>(plot.y + plot.height) + 4;
    const char* firstLabel = TextFormat("t%llu", static_cast<unsigned long long>(history.front().tick));
    const int firstWidth = MeasureText(firstLabel, kAxisFont);
    DrawText(firstLabel, static_cast<int>(plot.x), axisY, kAxisFont, kMutedColor);
    const char* lastLabel = TextFormat("t%llu", static_cast<unsigned long long>(history.back().tick));
    const int lastWidth = MeasureText(lastLabel, kAxisFont);
    DrawText(lastLabel, static_cast<int>(plot.x + plot.width) - lastWidth, axisY, kAxisFont, kMutedColor);

    if (footerNote != nullptr) {
        const int noteWidth = MeasureText(footerNote, kAxisFont);
        if (firstWidth + lastWidth + noteWidth + 24 < static_cast<int>(plot.width)) {
            DrawText(footerNote, static_cast<int>(plot.x + plot.width * 0.5f) - noteWidth / 2, axisY, kAxisFont,
                     kMutedColor);
        }
    }
}

} // namespace

void draw(const WorldState& state, Rectangle bounds, bool allowHover) {
    DrawRectangleRec(bounds, Color{16, 16, 20, 238});
    DrawRectangleLinesEx(bounds, 1.0f, Color{60, 60, 68, 255});

    const History& history = state.populationHistory;
    if (history.size() < 2) {
        DrawText(state.connected ? "Genome history: waiting for the world to tick"
                                  : "Genome history: not connected to the server",
                 static_cast<int>(bounds.x) + 10, static_cast<int>(bounds.y) + 10, kTitleFont, kMutedColor);
        return;
    }

    constexpr float kPadding = 10.0f;
    constexpr float kGap = 14.0f;
    // Три панели, как и у численности, и по той же причине: у травы,
    // травоядных и хищников свои таблицы черт, и одна общая легенда на
    // сорок линий не читалась бы вовсе.
    const float chartWidth = bounds.width - kPadding * 2.0f;
    // Пять панелей: трава, рощи, травоядные, хищники, гоблины.
    const float chartHeight = (bounds.height - kPadding * 2.0f - kGap * 4.0f) / 5.0f;
    if (chartWidth < kValueLabelWidth + 60.0f || chartHeight < kHeaderHeight + kFooterHeight + 24.0f) {
        DrawText("Genome history: panel is too small", static_cast<int>(bounds.x) + 10,
                 static_cast<int>(bounds.y) + 10, kTitleFont, kMutedColor);
        return;
    }
    const Rectangle plants{bounds.x + kPadding, bounds.y + kPadding, chartWidth, chartHeight};
    const Rectangle trees{plants.x, plants.y + chartHeight + kGap, chartWidth, chartHeight};
    const Rectangle herbivores{plants.x, trees.y + chartHeight + kGap, chartWidth, chartHeight};
    const Rectangle predators{plants.x, herbivores.y + chartHeight + kGap, chartWidth, chartHeight};
    const Rectangle goblinsChart{plants.x, predators.y + chartHeight + kGap, chartWidth, chartHeight};

    // Курсор один на все панели: они стоят одна под другой, по
    // горизонтали совпадают, и вопрос к ним общий — что было со всеми
    // тремя геномами в один и тот же момент.
    bool hasHover = false;
    std::size_t hoverIndex = 0;
    if (allowHover) {
        const Vector2 mouse = GetMousePosition();
        if (CheckCollisionPointRec(mouse, bounds)) {
            const Rectangle plot = plotOf(plants);
            if (mouse.x >= plot.x && mouse.x <= plot.x + plot.width) {
                hoverIndex = nearestPoint(history, plot, mouse.x);
                hasHover = true;
            }
        }
    }

    // Подпись про шкалу — под нижним графиком: она относится ко всем
    // пяти, и повторять её пять раз незачем.
    drawChart(history, state.plantTraits, plants, Kind::Plants, "Grass -- average genome", hasHover, hoverIndex,
              nullptr);
    // У дерева таблица черт своя (пределы те же черты меряют иначе:
    // max_age травы — сотни тиков, дерева — десятки тысяч), поэтому и
    // панель своя, со своей легендой.
    drawChart(history, state.treeTraits, trees, Kind::Trees, "Trees -- average genome", hasHover, hoverIndex,
              nullptr);
    drawChart(history, state.herbivoreTraits, herbivores, Kind::Herbivores, "Herbivores -- average genome", hasHover,
              hoverIndex, nullptr);
    drawChart(history, state.predatorTraits, predators, Kind::Predators, "Predators -- average genome", hasHover,
              hoverIndex, nullptr);
    // У гоблина таблица черт своя, и трёх звериных черт в ней нет вовсе
    // (зубы, рога, меткость рогов) — панель своя, со своей легендой.
    drawChart(history, state.goblinTraits, goblinsChart, Kind::Goblins, "Goblins -- average genome", hasHover,
              hoverIndex, "0% = worst value of the trait, 100% = best");
}

} // namespace GenomeGraph

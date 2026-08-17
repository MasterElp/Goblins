#include "PopulationGraph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <raylib.h>

#include "TileColors.hpp"

namespace PopulationGraph {

namespace {

constexpr int kTitleFont = 14;
constexpr int kAxisFont = 12;

// Разметка панели одного графика: слева — подписи значений, сверху —
// заголовок и строка легенды, снизу — подписи тиков. Числа нужны и
// отрисовке, и поиску точки под курсором, поэтому поле графика считает
// одна функция (plotOf ниже), а не каждое место по-своему.
constexpr float kValueLabelWidth = 46.0f;
constexpr float kHeaderHeight = 36.0f;
constexpr float kFooterHeight = 17.0f;

using History = std::vector<WorldState::PopulationPoint>;

enum class Kind { Plants, Herbivores, Predators };

const std::vector<int>& valuesOf(const WorldState::PopulationPoint& point, Kind kind) {
    switch (kind) {
        case Kind::Plants: return point.plants;
        case Kind::Predators: return point.predators;
        case Kind::Herbivores: break;
    }
    return point.herbivores;
}

// Сумма по всем видам этой панели.
int totalOf(const WorldState::PopulationPoint& point, Kind kind) {
    int total = 0;
    for (const int value : valuesOf(point, kind)) {
        total += value;
    }
    return total;
}

// Линия суммы — светлее и толще любой видовой: это не ещё один вид, а
// итог, и путать их взглядом нельзя.
constexpr Color kTotalColor{235, 235, 240, 255};

// Цвета — те же, что у видов на карте и в легенде экрана мира: линия
// графика и пятно травы на карте должны читаться как одно и то же.
Color seriesColor(Kind kind, int species) {
    switch (kind) {
        case Kind::Plants: return TileColors::plantSpecies(species);
        case Kind::Predators: return TileColors::predatorSpecies(species);
        case Kind::Herbivores: break;
    }
    return TileColors::herbivoreSpecies(species);
}

// Круглый потолок шкалы: 1, 2, 5, 10, 20, 50, ... Ось с потолком "по
// максимуму" (3847) читалась бы хуже, чем с 5000, и прыгала бы на каждой
// новой точке.
int niceCeil(int value) {
    if (value <= 5) {
        return std::max(value, 1);
    }
    const double magnitude = std::pow(10.0, std::floor(std::log10(static_cast<double>(value))));
    for (const double step : {1.0, 2.0, 5.0, 10.0}) {
        const double candidate = step * magnitude;
        if (candidate >= static_cast<double>(value)) {
            return static_cast<int>(candidate);
        }
    }
    return value;
}

Rectangle plotOf(Rectangle area) {
    return Rectangle{area.x + kValueLabelWidth, area.y + kHeaderHeight,
                     std::max(1.0f, area.width - kValueLabelWidth - 6.0f),
                     std::max(1.0f, area.height - kHeaderHeight - kFooterHeight)};
}

// Ось времени — по тику, а не по номеру точки: точки летописи стоят через
// равный шаг тиков, но при прореживании (см. PopulationHistory::thin)
// старая половина идёт с одним шагом, а новая — с другим, и по номерам
// время шло бы неравномерно. Вызывать только при непустой летописи.
float xAt(const History& history, const Rectangle& plot, std::uint64_t tick) {
    const std::uint64_t first = history.front().tick;
    const std::uint64_t last = history.back().tick;
    const double span = last > first ? static_cast<double>(last - first) : 1.0;
    const double t = std::clamp(static_cast<double>(tick - first) / span, 0.0, 1.0);
    return plot.x + static_cast<float>(t) * plot.width;
}

float yAt(const Rectangle& plot, int top, int value) {
    const float t = top > 0 ? std::clamp(static_cast<float>(value) / static_cast<float>(top), 0.0f, 1.0f) : 0.0f;
    return plot.y + plot.height - t * plot.height;
}

// Точка летописи, ближайшая к этому месту по горизонтали. Вызывать только
// при непустой летописи.
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

// Один график. hasHover/hoverIndex — общая для обеих панелей точка под
// курсором: смотреть на траву и на стадо в один и тот же момент — ровно
// то, ради чего этот курсор и нужен. footerNote рисуется по центру нижней
// строки, если влезает (см. draw ниже).
void drawChart(const History& history, Rectangle area, Kind kind, const char* title, bool hasHover,
               std::size_t hoverIndex, const char* footerNote) {
    const Color titleColor{200, 200, 205, 255};
    const Color mutedColor{130, 130, 138, 255};
    const Color gridColor{52, 52, 60, 255};
    const Color cursorColor{255, 220, 90, 255};

    DrawText(title, static_cast<int>(area.x), static_cast<int>(area.y), kTitleFont, titleColor);

    // Числа в легенде — из показываемого момента: под курсором это его
    // точка, иначе последняя пришедшая.
    const WorldState::PopulationPoint& shown = hasHover ? history[hoverIndex] : history.back();
    const char* tickLabel = hasHover ? TextFormat("tick %llu", static_cast<unsigned long long>(shown.tick))
                                      : TextFormat("tick %llu (now)", static_cast<unsigned long long>(shown.tick));
    DrawText(tickLabel, static_cast<int>(area.x + area.width) - MeasureText(tickLabel, kAxisFont),
             static_cast<int>(area.y) + 2, kAxisFont, hasHover ? cursorColor : mutedColor);

    const Rectangle plot = plotOf(area);
    if (plot.width < 40.0f || plot.height < 30.0f) {
        return;
    }

    // Число рядов — по последней точке: виды не появляются и не исчезают
    // без регенерации, а она заводит новую летопись (вымерший вид остаётся
    // в ней нулём, а не пропадает из точки).
    const std::size_t seriesCount = valuesOf(history.back(), kind).size();
    if (seriesCount == 0) {
        DrawText("none in this world", static_cast<int>(plot.x) + 6, static_cast<int>(plot.y) + 6, kAxisFont,
                 mutedColor);
        return;
    }

    const auto& values = valuesOf(shown, kind);
    int legendX = static_cast<int>(area.x);
    const int legendY = static_cast<int>(area.y) + kTitleFont + 5;
    // Сумма по всем видам — первой в легенде и отдельной линией на
    // графике: по видам видно, кто кого вытесняет, а по сумме — держится
    // ли мир вообще. Одно другого не заменяет: три вида, меняющиеся
    // местами, дают ровную сумму, а три вымирающих — падающую при точно
    // так же пересекающихся кривых.
    {
        const char* label = TextFormat("total %d", totalOf(shown, kind));
        DrawRectangle(legendX, legendY + 1, 9, 9, kTotalColor);
        DrawText(label, legendX + 13, legendY, kAxisFont, titleColor);
        legendX += 13 + MeasureText(label, kAxisFont) + 10;
    }
    for (std::size_t s = 0; s < seriesCount; ++s) {
        const char* label = TextFormat("%d", s < values.size() ? values[s] : 0);
        const int entryWidth = 13 + MeasureText(label, kAxisFont) + 10;
        // Видов может быть двенадцать, а панель — вдвое уже окна: лучше
        // оборвать легенду, чем налезть на соседний график.
        if (legendX + entryWidth > static_cast<int>(area.x + area.width)) {
            break;
        }
        DrawRectangle(legendX, legendY + 1, 9, 9, seriesColor(kind, static_cast<int>(s)));
        DrawText(label, legendX + 13, legendY, kAxisFont, titleColor);
        legendX += entryWidth;
    }

    // Шкала — общая для всех видов панели (у каждого своя она означала бы,
    // что рядом лежащие линии несравнимы) и по максимуму за всю летопись, а
    // не за видимый хвост: иначе спад популяции выглядел бы как рост фона.
    // Потолок — по сумме, а не по самому многочисленному виду: линия суммы
    // всегда выше любой отдельной, и шкала обязана вмещать её целиком.
    int maxValue = 0;
    for (const auto& point : history) {
        maxValue = std::max(maxValue, totalOf(point, kind));
    }
    const int top = niceCeil(std::max(maxValue, 1));

    for (int step = 0; step <= 2; ++step) {
        const int value = top * step / 2;
        const int y = static_cast<int>(yAt(plot, top, value));
        DrawLine(static_cast<int>(plot.x), y, static_cast<int>(plot.x + plot.width), y, gridColor);
        const char* label = TextFormat("%d", value);
        DrawText(label, static_cast<int>(plot.x) - 6 - MeasureText(label, kAxisFont), y - kAxisFont / 2, kAxisFont,
                 mutedColor);
    }

    for (std::size_t s = 0; s < seriesCount; ++s) {
        const Color color = seriesColor(kind, static_cast<int>(s));
        Vector2 previous{};
        bool hasPrevious = false;
        for (const auto& point : history) {
            const auto& pointValues = valuesOf(point, kind);
            if (s >= pointValues.size()) {
                hasPrevious = false;
                continue;
            }
            const Vector2 screen{xAt(history, plot, point.tick), yAt(plot, top, pointValues[s])};
            if (hasPrevious) {
                DrawLineEx(previous, screen, 1.5f, color);
            }
            previous = screen;
            hasPrevious = true;
        }
    }

    // Сумма — последней, поверх видовых линий: она итог, и перекрывать её
    // не должно ничто.
    {
        Vector2 previous{};
        bool hasPrevious = false;
        for (const auto& point : history) {
            const Vector2 screen{xAt(history, plot, point.tick), yAt(plot, top, totalOf(point, kind))};
            if (hasPrevious) {
                DrawLineEx(previous, screen, 2.0f, kTotalColor);
            }
            previous = screen;
            hasPrevious = true;
        }
    }

    if (hasHover) {
        const float x = xAt(history, plot, shown.tick);
        DrawLine(static_cast<int>(x), static_cast<int>(plot.y), static_cast<int>(x),
                 static_cast<int>(plot.y + plot.height), Color{255, 220, 90, 120});
        for (std::size_t s = 0; s < seriesCount && s < values.size(); ++s) {
            DrawCircle(static_cast<int>(x), static_cast<int>(yAt(plot, top, values[s])), 2.5f,
                       seriesColor(kind, static_cast<int>(s)));
        }
        DrawCircle(static_cast<int>(x), static_cast<int>(yAt(plot, top, totalOf(shown, kind))), 3.0f, kTotalColor);
    }

    const int axisY = static_cast<int>(plot.y + plot.height) + 4;
    const char* firstLabel = TextFormat("t%llu", static_cast<unsigned long long>(history.front().tick));
    const int firstWidth = MeasureText(firstLabel, kAxisFont);
    DrawText(firstLabel, static_cast<int>(plot.x), axisY, kAxisFont, mutedColor);
    const char* lastLabel = TextFormat("t%llu", static_cast<unsigned long long>(history.back().tick));
    const int lastWidth = MeasureText(lastLabel, kAxisFont);
    DrawText(lastLabel, static_cast<int>(plot.x + plot.width) - lastWidth, axisY, kAxisFont, mutedColor);

    if (footerNote != nullptr) {
        const int noteWidth = MeasureText(footerNote, kAxisFont);
        if (firstWidth + lastWidth + noteWidth + 24 < static_cast<int>(plot.width)) {
            DrawText(footerNote, static_cast<int>(plot.x + plot.width * 0.5f) - noteWidth / 2, axisY, kAxisFont,
                     mutedColor);
        }
    }
}

} // namespace

void draw(const WorldState& state, Rectangle bounds, bool allowHover) {
    // Подложка почти непрозрачная: тонкие линии графика поверх
    // просвечивающей карты не читаются (как и текст оверлея констант).
    DrawRectangleRec(bounds, Color{16, 16, 20, 238});
    DrawRectangleLinesEx(bounds, 1.0f, Color{60, 60, 68, 255});

    const Color mutedColor{130, 130, 138, 255};
    const History& history = state.populationHistory;
    if (history.size() < 2) {
        // Летопись новорождённого мира состоит из одной точки: линию по
        // ней не провести, и честнее сказать, чего ждём.
        DrawText(state.connected ? "Population history: waiting for the world to tick"
                                  : "Population history: not connected to the server",
                 static_cast<int>(bounds.x) + 10, static_cast<int>(bounds.y) + 10, kTitleFont, mutedColor);
        return;
    }

    constexpr float kPadding = 10.0f;
    constexpr float kGap = 14.0f;
    // Три панели, а не одна: у травы, травоядных и хищников свои единицы и
    // свои порядки величины (тысячи клеток против десятков особей), и на
    // одной шкале кривая хищников легла бы в ноль. Друг под другом, а не в
    // ряд: панель узкая и высокая (она стоит справа от карты), и три
    // графика по трети её ширины были бы шириной с подпись.
    const float chartWidth = bounds.width - kPadding * 2.0f;
    const float chartHeight = (bounds.height - kPadding * 2.0f - kGap * 2.0f) / 3.0f;
    if (chartWidth < kValueLabelWidth + 60.0f || chartHeight < kHeaderHeight + kFooterHeight + 30.0f) {
        DrawText("Population history: panel is too small", static_cast<int>(bounds.x) + 10,
                 static_cast<int>(bounds.y) + 10, kTitleFont, mutedColor);
        return;
    }
    const Rectangle plants{bounds.x + kPadding, bounds.y + kPadding, chartWidth, chartHeight};
    const Rectangle herbivores{plants.x, plants.y + chartHeight + kGap, chartWidth, chartHeight};
    const Rectangle predators{plants.x, herbivores.y + chartHeight + kGap, chartWidth, chartHeight};

    // Курсор один на все панели: точка летописи общая, и вопрос, ради
    // которого на график вообще смотрят, — что было с травой в тот момент,
    // когда просело стадо, и что было со стадом, когда расплодились
    // хищники. Панели стоят одна под другой, поэтому по горизонтали они
    // совпадают, и достаточно поля любой из них.
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

    // Сколько точек и с каким шагом — иначе прореживание летописи на
    // сервере выглядело бы как замедлившееся время. Собственной строкой, а
    // не указателем от TextFormat: тот отдаёт один из нескольких
    // статических буферов по кругу, а до места отрисовки (внутри
    // drawChart) успевает пройти не один десяток других вызовов.
    const std::string footerNote =
        state.populationInterval > 1
            ? std::string(TextFormat("%zu points, 1 per %llu ticks", history.size(),
                                      static_cast<unsigned long long>(state.populationInterval)))
            : std::string(TextFormat("%zu points", history.size()));

    drawChart(history, plants, Kind::Plants, "Grass -- tiles", hasHover, hoverIndex, nullptr);
    drawChart(history, herbivores, Kind::Herbivores, "Herbivores -- animals", hasHover, hoverIndex, nullptr);
    // Подпись про число точек — под нижним графиком: она относится ко всей
    // летописи, а не к одной панели, и повторять её трижды незачем.
    drawChart(history, predators, Kind::Predators, "Predators -- animals", hasHover, hoverIndex, footerNote.c_str());
}

} // namespace PopulationGraph

#include "ConstantsOverlay.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <raylib.h>

namespace ConstantsOverlay {

namespace {

constexpr int kFontSize = 14;
constexpr int kLineHeight = 18;
constexpr int kGroupGap = 10;
constexpr int kColumnWidth = 300;
constexpr int kMargin = 24;
constexpr int kHeaderHeight = 46;

// Значения тут разного порядка — от 0.00004 (испарение) до 4000 (потолок
// длины пути реки). Один общий формат либо съел бы малые в ноль, либо
// растянул большие на десяток знаков, поэтому формат выбирается по самому
// числу. Целые печатаются целыми: "kSeedMinFit: 0.2500" читается как
// дробь, а "kSpeciesAttempts: 200.0000" — как ошибка.
std::string formatValue(float value) {
    const float magnitude = std::fabs(value);
    if (value == std::floor(value) && magnitude < 1e7f) {
        return TextFormat("%.0f", value);
    }
    if (magnitude < 0.001f) {
        return TextFormat("%.6f", value);
    }
    if (magnitude < 1.0f) {
        return TextFormat("%.4f", value);
    }
    return TextFormat("%.3f", value);
}

// Состояние оверлея — на весь клиент одно (см. ConstantsOverlay.hpp).
bool g_visible = false;

} // namespace

bool visible() {
    return g_visible;
}

void update(const WorldState& state, bool allowToggle) {
    if (allowToggle && IsKeyPressed(KEY_C)) {
        g_visible = !g_visible;
    }
    if (!g_visible) {
        return;
    }

    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();

    // Подложка почти непрозрачная: это плотный текст мелким кеглем, и
    // просвечивающая сквозь него карта делает его нечитаемым.
    DrawRectangle(0, 0, screenW, screenH, Color{16, 16, 20, 242});

    const Color titleColor{235, 200, 110, 255};
    const Color groupColor{150, 210, 255, 255};
    const Color nameColor{200, 200, 205, 255};
    const Color valueColor{245, 245, 245, 255};
    const Color mutedColor{140, 140, 148, 255};

    DrawText("World law constants -- read-only, compiled into core", kMargin, 14, 20, titleColor);
    DrawText("These are NOT settings: they are the same for every world. Press C to close.", kMargin, 34, kFontSize,
             mutedColor);

    if (state.constants.empty()) {
        DrawText(state.connected ? "No constants received from the server yet."
                                  : "Not connected to the server.",
                 kMargin, kHeaderHeight + 20, kFontSize, mutedColor);
        return;
    }

    // Раскладка по колонкам: сколько строк влезает в высоту окна, столько
    // и кладём в колонку, дальше начинаем следующую. Группа переносится
    // целиком, если её хвост не влезает — разорванная посередине группа
    // читается как две разные.
    const int firstLineY = kHeaderHeight + 12;
    const int linesPerColumn = std::max(1, (screenH - firstLineY - kMargin) / kLineHeight);

    int column = 0;
    int line = 0;
    std::string currentGroup;

    for (std::size_t i = 0; i < state.constants.size(); ++i) {
        const auto& constant = state.constants[i];

        if (constant.group != currentGroup) {
            // Сколько строк займёт эта группа целиком (заголовок + её
            // константы) — чтобы решить, начинать ли новую колонку.
            int groupLines = 1;
            for (std::size_t j = i; j < state.constants.size() && state.constants[j].group == constant.group; ++j) {
                ++groupLines;
            }
            const bool needsGap = line > 0;
            const int cost = groupLines + (needsGap ? 1 : 0);
            if (line > 0 && line + cost > linesPerColumn) {
                ++column;
                line = 0;
            } else if (needsGap) {
                line += 1;
            }
            currentGroup = constant.group;

            const int x = kMargin + column * kColumnWidth;
            const int y = firstLineY + line * kLineHeight + (line > 0 ? kGroupGap : 0);
            DrawText(currentGroup.c_str(), x, y, kFontSize, groupColor);
            ++line;
        }

        const int x = kMargin + column * kColumnWidth;
        const int y = firstLineY + line * kLineHeight;
        DrawText(constant.name.c_str(), x + 8, y, kFontSize, nameColor);
        const std::string text = formatValue(constant.value);
        const int textWidth = MeasureText(text.c_str(), kFontSize);
        DrawText(text.c_str(), x + kColumnWidth - 20 - textWidth, y, kFontSize, valueColor);
        ++line;
    }

    DrawText(TextFormat("%d constants", static_cast<int>(state.constants.size())), kMargin, screenH - 20, kFontSize,
             mutedColor);
}

} // namespace ConstantsOverlay

#include "KeysPanel.hpp"

#include <raylib.h>

namespace KeysPanel {

namespace {

constexpr int kFont = 14;
constexpr int kLineHeight = 19;
constexpr int kGroupGap = 10;
// Ширина колонки с клавишей. Подобрана под самую длинную из них
// ("W A S D / arrows"): выравнивать описания по ней — единственный способ
// читать список глазами, а не разбирать каждую строку отдельно.
constexpr float kKeyColumnWidth = 150.0f;

const Color kGroupColor{150, 210, 255, 255};
const Color kKeyColor{235, 200, 110, 255};
const Color kTextColor{200, 200, 205, 255};
const Color kOnColor{160, 220, 150, 255};
const Color kOffColor{120, 120, 126, 255};

} // namespace

void draw(Rectangle bounds, const MapTexture::Layers& layers) {
    float y = bounds.y;
    const int x = static_cast<int>(bounds.x);
    const int textX = static_cast<int>(bounds.x + kKeyColumnWidth);

    const auto group = [&](const char* title) {
        if (y > bounds.y) {
            y += kGroupGap;
        }
        DrawText(title, x, static_cast<int>(y), kFont, kGroupColor);
        y += kLineHeight;
    };
    const auto row = [&](const char* key, const char* what) {
        DrawText(key, x + 6, static_cast<int>(y), kFont, kKeyColor);
        DrawText(what, textX, static_cast<int>(y), kFont, kTextColor);
        y += kLineHeight;
    };
    // Слой — та же строка, но с состоянием справа: включён он или нет,
    // видно на месте, без похода на карту.
    const auto layerRow = [&](const char* key, const char* what, bool enabled) {
        DrawText(key, x + 6, static_cast<int>(y), kFont, kKeyColor);
        DrawText(what, textX, static_cast<int>(y), kFont, enabled ? kTextColor : kOffColor);
        const char* state = enabled ? "on" : "off";
        DrawText(state, static_cast<int>(bounds.x + bounds.width) - MeasureText(state, kFont) - 6,
                 static_cast<int>(y), kFont, enabled ? kOnColor : kOffColor);
        y += kLineHeight;
    };

    group("Map");
    row("W A S D / arrows", "scroll");
    row("Mouse wheel", "zoom to cursor");
    row("F", "fit the whole map in the window");

    group("Selection");
    row("Left click", "inspect tile; click again to cycle");
    row("", "creatures -> grass -> soil");
    row("Right click", "stop following");

    group("Layers -- what the tile colour is made of");
    layerRow("1", "Rockiness", layers.rockiness);
    layerRow("2", "Moisture + water", layers.moisture);
    layerRow("3", "Minerals", layers.minerals);
    layerRow("4", "Height shading", layers.height);
    layerRow("5", "Grass + humus + seeds", layers.plants);
    layerRow("6", "Animals + carcasses", layers.animals);

    group("World");
    row("Space", "pause / resume (the world stops)");
    row("C", "world law constants (read-only)");

    group("Panel");
    row("P", "next tab, then hide the panel");
    row("", "Info -> Params -> Graphs -> Keys");

    group("Leaving");
    row("Esc", "back to the menu (asks to save)");
    row("Window close", "asks to save as well");
}

} // namespace KeysPanel

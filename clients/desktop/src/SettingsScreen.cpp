#include "SettingsScreen.hpp"

#include <array>

#include <raygui.h>
#include <raylib.h>

namespace SettingsScreen {

namespace {

struct Preset {
    int width;
    int height;
    const char* label;
};

constexpr std::array<Preset, 4> kPresets{{
    {1280, 720, "1280 x 720"},
    {1600, 900, "1600 x 900"},
    {1920, 1080, "1920 x 1080"},
    {2560, 1440, "2560 x 1440"},
}};

} // namespace

AppScreen draw(goblins::ClientConfig& config, const std::string& configPath) {
    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();

    ClearBackground(Color{28, 28, 32, 255});

    const Color textColor{230, 230, 230, 255};
    const Color mutedColor{160, 160, 160, 255};

    DrawText("Settings", 30, 24, 28, textColor);

    const float panelX = 40.0f;
    float y = 84.0f;

    DrawText("Resolution (windowed mode)", static_cast<int>(panelX), static_cast<int>(y), 18, mutedColor);
    y += 30.0f;

    for (const auto& preset : kPresets) {
        const bool isCurrent = (config.window_width == preset.width && config.window_height == preset.height);
        if (isCurrent) {
            // Визуально выделяем уже применённый пресет — GuiSetState
            // рисует кнопку в disabled-стиле (просто как индикатор, не
            // настоящая блокировка: клик по уже текущему пресету ничего
            // не меняет и без этого).
            GuiSetState(STATE_DISABLED);
        }
        if (GuiButton(Rectangle{panelX, y, 220, 32}, preset.label) && !isCurrent) {
            config.window_width = preset.width;
            config.window_height = preset.height;
            if (!config.fullscreen) {
                SetWindowSize(preset.width, preset.height);
            }
            goblins::saveClientConfig(configPath, config);
        }
        if (isCurrent) {
            GuiSetState(STATE_NORMAL);
        }
        y += 40.0f;
    }

    y += 20.0f;
    bool fullscreen = config.fullscreen;
    GuiCheckBox(Rectangle{panelX, y, 20, 20}, "Fullscreen (exclusive)", &fullscreen);
    if (fullscreen != config.fullscreen) {
        config.fullscreen = fullscreen;
        ToggleFullscreen();
        goblins::saveClientConfig(configPath, config);
    }
    y += 40.0f;

    DrawText(TextFormat("Current window: %d x %d  (%s)", screenW, screenH,
                         IsWindowFullscreen() ? "fullscreen" : "windowed"),
             static_cast<int>(panelX), static_cast<int>(y), 16, mutedColor);

    if (GuiButton(Rectangle{30, static_cast<float>(screenH) - 60, 140, 34}, "Back (Esc)")) {
        return AppScreen::MainMenu;
    }

    return AppScreen::Settings;
}

} // namespace SettingsScreen

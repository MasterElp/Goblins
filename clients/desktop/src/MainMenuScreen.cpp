#include "MainMenuScreen.hpp"

#include <raygui.h>
#include <raylib.h>

namespace MainMenuScreen {

AppScreen draw(NetworkClient& network) {
    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();

    ClearBackground(Color{28, 28, 32, 255});

    const char* title = "Goblins - World Simulator";
    const int titleSize = 32;
    const int titleWidth = MeasureText(title, titleSize);
    DrawText(title, screenW / 2 - titleWidth / 2, screenH / 3 - 60, titleSize, Color{230, 230, 230, 255});

    const float buttonWidth = 260.0f;
    const float buttonHeight = 44.0f;
    const float gap = 16.0f;
    const float startY = static_cast<float>(screenH) / 3.0f;
    const float x = static_cast<float>(screenW) / 2.0f - buttonWidth / 2.0f;

    AppScreen next = AppScreen::MainMenu;

    if (GuiButton(Rectangle{x, startY, buttonWidth, buttonHeight}, "Settings")) {
        next = AppScreen::Settings;
    }
    // Один пункт вместо прежних "World Generation" и "Simulation": и
    // подбор параметров генерации, и наблюдение за симуляцией теперь на
    // одном экране (см. AppScreen.hpp). Мир — всегда конкретный
    // сохранённый мир, поэтому сначала экран выбора. Список запрашиваем
    // сразу, чтобы он был на месте к первому же кадру нового экрана.
    if (GuiButton(Rectangle{x, startY + (buttonHeight + gap), buttonWidth, buttonHeight}, "World")) {
        network.sendListWorlds();
        next = AppScreen::WorldSelect;
    }

    return next;
}

} // namespace MainMenuScreen

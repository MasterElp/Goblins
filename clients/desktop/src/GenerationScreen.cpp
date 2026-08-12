#include "GenerationScreen.hpp"

#include <algorithm>
#include <cmath>

#include <raygui.h>
#include <raylib.h>

#include "TileColors.hpp"

namespace GenerationScreen {

namespace {
constexpr float kPanelWidth = 400.0f;
constexpr float kTopBarHeight = 40.0f;
} // namespace

AppScreen draw(NetworkClient& network, SettingsPanel& panel) {
    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();

    ClearBackground(Color{28, 28, 32, 255});

    const Color textColor{230, 230, 230, 255};
    const Color mutedColor{160, 160, 160, 255};
    const Color cursorColor{255, 220, 90, 255};

    DrawText("World Generation - Stage 1: Soil & Water", 12, 10, 20, textColor);

    // Сохранить то, что сейчас на экране, как отдельный мир: только что
    // сгенерированный мир всегда стоит на нулевом тике, поэтому в списке
    // миров он появится ровно в том состоянии, в каком его видно здесь.
    if (GuiButton(Rectangle{static_cast<float>(screenW) - kPanelWidth - 240, 6, 120, 28}, "Save as world")) {
        network.sendSaveWorld();
    }
    if (GuiButton(Rectangle{static_cast<float>(screenW) - kPanelWidth - 110, 6, 100, 28}, "Back (Esc)")) {
        return AppScreen::MainMenu;
    }

    const WorldState snapshot = network.snapshot();
    if (snapshot.hasGeneration) {
        panel.loadFrom(snapshot.generation);
    }

    // Область под карту — всё окно за вычетом верхней полосы и правой
    // панели настроек.
    const float mapAreaX = 0.0f;
    const float mapAreaY = kTopBarHeight;
    const float mapAreaW = std::max(0.0f, static_cast<float>(screenW) - kPanelWidth);
    const float mapAreaH = std::max(0.0f, static_cast<float>(screenH) - kTopBarHeight);

    bool hasHoverTile = false;
    int hoverX = 0;
    int hoverY = 0;

    if (!snapshot.connected || snapshot.areaWidth == 0) {
        const char* waiting = "Connecting to server...";
        const int labelWidth = MeasureText(waiting, 20);
        DrawText(waiting, static_cast<int>(mapAreaX + mapAreaW / 2 - labelWidth / 2),
                 static_cast<int>(mapAreaY + mapAreaH / 2), 20, mutedColor);
    } else {
        // Размер тайла — под текущее окно, вся карта целиком, без
        // прокрутки. Пересчитывается каждый кадр, поэтому при
        // изменении размера окна меняется только отображение.
        const float tileSizeF =
            std::max(1.0f, std::floor(std::min(mapAreaW / snapshot.areaWidth, mapAreaH / snapshot.areaHeight)));
        const int tileSize = static_cast<int>(tileSizeF);

        const float mapPixelW = tileSizeF * snapshot.areaWidth;
        const float mapPixelH = tileSizeF * snapshot.areaHeight;
        const float offsetX = mapAreaX + (mapAreaW - mapPixelW) / 2.0f;
        const float offsetY = mapAreaY + (mapAreaH - mapPixelH) / 2.0f;

        BeginScissorMode(static_cast<int>(mapAreaX), static_cast<int>(mapAreaY), static_cast<int>(mapAreaW),
                          static_cast<int>(mapAreaH));

        for (int y = 0; y < snapshot.areaHeight; ++y) {
            for (int x = 0; x < snapshot.areaWidth; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * snapshot.areaWidth + x;
                const float screenX = offsetX + x * tileSizeF;
                const float screenY = offsetY + y * tileSizeF;

                DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                              TileColors::soil(snapshot.moisture[i], snapshot.rockiness[i], snapshot.compaction[i]));
                if (snapshot.waterDepth[i] > 0.0f) {
                    DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                                  TileColors::water(snapshot.waterDepth[i]));
                }
            }
        }

        for (const auto& boulder : snapshot.boulders) {
            const float screenX = offsetX + boulder.first * tileSizeF;
            const float screenY = offsetY + boulder.second * tileSizeF;
            const int inset = tileSize > 3 ? 1 : 0;
            DrawRectangle(static_cast<int>(screenX) + inset, static_cast<int>(screenY) + inset,
                          std::max(1, tileSize - 2 * inset), std::max(1, tileSize - 2 * inset),
                          Color{70, 66, 62, 255});
        }

        const Vector2 mouse = GetMousePosition();
        if (mouse.x >= offsetX && mouse.x < offsetX + mapPixelW && mouse.y >= offsetY && mouse.y < offsetY + mapPixelH) {
            hoverX = static_cast<int>((mouse.x - offsetX) / tileSizeF);
            hoverY = static_cast<int>((mouse.y - offsetY) / tileSizeF);
            hasHoverTile = hoverX >= 0 && hoverX < snapshot.areaWidth && hoverY >= 0 && hoverY < snapshot.areaHeight;
            if (hasHoverTile) {
                DrawRectangleLines(static_cast<int>(offsetX + hoverX * tileSizeF),
                                    static_cast<int>(offsetY + hoverY * tileSizeF), tileSize, tileSize, cursorColor);
            }
        }

        EndScissorMode();

        DrawText(TextFormat("Area: %dx%d   Tile: %dpx", snapshot.areaWidth, snapshot.areaHeight, tileSize), 12,
                 static_cast<int>(kTopBarHeight) + 4, 14, mutedColor);
    }

    if (hasHoverTile) {
        const std::size_t hi = static_cast<std::size_t>(hoverY) * snapshot.areaWidth + hoverX;
        const std::string label =
            TextFormat("Tile (%d,%d)  moist %.2f  rock %.2f  pack %.2f%s", hoverX, hoverY, snapshot.moisture[hi],
                       snapshot.rockiness[hi], snapshot.compaction[hi],
                       snapshot.waterDepth[hi] > 0.0f ? TextFormat("  water %.2f", snapshot.waterDepth[hi]) : "");
        DrawText(label.c_str(), 12, screenH - 22, 16, cursorColor);
    }

    // Ответ сервера на "Save as world" — иначе кнопка выглядела бы
    // ничего не делающей.
    if (hasFreshNotice(snapshot)) {
        DrawText(snapshot.notice.c_str(), 12, screenH - 44, 16,
                 snapshot.noticeIsError ? Color{230, 110, 110, 255} : mutedColor);
    }

    goblins::RegenerationRequest regenerateRequest;
    bool saveRequested = false;
    const Rectangle panelBounds{static_cast<float>(screenW) - kPanelWidth, 0, kPanelWidth,
                                 static_cast<float>(screenH)};
    if (panel.draw(panelBounds, regenerateRequest, saveRequested)) {
        network.sendRegenerate(regenerateRequest);
    }
    if (saveRequested) {
        network.sendSaveGenerationConfig();
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        return AppScreen::MainMenu;
    }
    return AppScreen::WorldGeneration;
}

} // namespace GenerationScreen

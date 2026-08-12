#include "SimulationScreen.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>
#include <raylib.h>

#include "TileColors.hpp"

namespace SimulationScreen {

namespace {
constexpr int kHudHeight = 32;
} // namespace

AppScreen draw(NetworkClient& network, const goblins::ClientConfig& config) {
    // Персистентны между кадрами, пока это состояние активно (при
    // возврате в меню и обратно позиция прокрутки сохраняется — это
    // осознанное поведение, не забытый сброс).
    static float viewX = 0.0f;
    static float viewY = 0.0f;

    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();
    const int tileSize = config.tile_size;
    const int viewportW = screenW;
    const int viewportH = screenH - kHudHeight;

    const Color backgroundColor{28, 28, 32, 255};
    const Color boulderColor{70, 66, 62, 255};
    const Color hudColor{18, 18, 20, 255};
    const Color textColor{230, 230, 230, 255};
    const Color cursorColor{255, 220, 90, 255};
    const Color pausedColor{220, 70, 70, 255};

    const float dt = GetFrameTime();
    const float scrollSpeedPx = static_cast<float>(config.scroll_step * tileSize);

    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) viewY -= scrollSpeedPx * dt;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) viewY += scrollSpeedPx * dt;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) viewX -= scrollSpeedPx * dt;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) viewX += scrollSpeedPx * dt;

    // Пауза — не локальное состояние клиента, а запрос серверу (настоящая
    // пауза мира). Сам клиент своё "paused" не выставляет — ждёт
    // подтверждения через pause_state/world_snapshot, чтобы все
    // подключённые клиенты видели одно и то же состояние.
    if (IsKeyPressed(KEY_P)) {
        network.sendTogglePause();
    }

    const WorldState snapshot = network.snapshot();

    if (snapshot.areaWidth > 0) {
        const float maxX = std::max(0.0f, static_cast<float>(snapshot.areaWidth * tileSize - viewportW));
        const float maxY = std::max(0.0f, static_cast<float>(snapshot.areaHeight * tileSize - viewportH));
        viewX = std::clamp(viewX, 0.0f, maxX);
        viewY = std::clamp(viewY, 0.0f, maxY);
    } else {
        viewX = std::max(0.0f, viewX);
        viewY = std::max(0.0f, viewY);
    }

    const Vector2 mouse = GetMousePosition();
    bool hasHoverTile = false;
    int hoverX = 0;
    int hoverY = 0;
    if (snapshot.areaWidth > 0 && mouse.x >= 0 && mouse.x < viewportW && mouse.y >= kHudHeight &&
        mouse.y < kHudHeight + viewportH) {
        hoverX = static_cast<int>(std::floor((mouse.x + viewX) / tileSize));
        hoverY = static_cast<int>(std::floor((mouse.y - kHudHeight + viewY) / tileSize));
        hasHoverTile = hoverX >= 0 && hoverX < snapshot.areaWidth && hoverY >= 0 && hoverY < snapshot.areaHeight;
    }

    ClearBackground(backgroundColor);

    if (!snapshot.connected || snapshot.areaWidth == 0) {
        const std::string waiting = "Connecting to " + config.host + ":" + std::to_string(config.port) + "...";
        DrawText(waiting.c_str(), 10, kHudHeight + 10, 20, textColor);
    } else {
        BeginScissorMode(0, kHudHeight, viewportW, viewportH);

        const int firstTileX = std::max(0, static_cast<int>(viewX) / tileSize);
        const int firstTileY = std::max(0, static_cast<int>(viewY) / tileSize);
        const int lastTileX = std::min(snapshot.areaWidth - 1, firstTileX + viewportW / tileSize + 1);
        const int lastTileY = std::min(snapshot.areaHeight - 1, firstTileY + viewportH / tileSize + 1);

        for (int ty = firstTileY; ty <= lastTileY; ++ty) {
            for (int tx = firstTileX; tx <= lastTileX; ++tx) {
                const std::size_t i = static_cast<std::size_t>(ty) * snapshot.areaWidth + tx;
                const float screenX = static_cast<float>(tx * tileSize) - viewX;
                const float screenY = static_cast<float>(ty * tileSize) - viewY + kHudHeight;

                DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                              TileColors::soil(snapshot.moisture[i], snapshot.rockiness[i], snapshot.compaction[i]));
                if (snapshot.waterDepth[i] > 0.0f) {
                    DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                                  TileColors::water(snapshot.waterDepth[i]));
                }
            }
        }

        for (const auto& boulder : snapshot.boulders) {
            const float screenX = static_cast<float>(boulder.first * tileSize) - viewX;
            const float screenY = static_cast<float>(boulder.second * tileSize) - viewY + kHudHeight;
            if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                screenY > viewportH + kHudHeight) {
                continue;
            }
            DrawRectangle(static_cast<int>(screenX) + 2, static_cast<int>(screenY) + 2, tileSize - 4, tileSize - 4,
                          boulderColor);
        }

        if (hasHoverTile) {
            const float screenX = static_cast<float>(hoverX * tileSize) - viewX;
            const float screenY = static_cast<float>(hoverY * tileSize) - viewY + kHudHeight;
            DrawRectangleLines(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize, cursorColor);
        }

        EndScissorMode();
    }

    DrawRectangle(0, 0, viewportW, kHudHeight, hudColor);
    DrawText(TextFormat("Tick: %llu   Area: %dx%d   View: (%d,%d)   WASD-scroll  P-pause  Esc-menu",
                         static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                         static_cast<int>(viewX / tileSize), static_cast<int>(viewY / tileSize)),
             10, 8, 16, textColor);

    if (hasHoverTile) {
        const std::size_t hi = static_cast<std::size_t>(hoverY) * snapshot.areaWidth + hoverX;
        const std::string tileLabel =
            TextFormat("Tile (%d,%d)  moist %.2f  rock %.2f  pack %.2f%s", hoverX, hoverY, snapshot.moisture[hi],
                       snapshot.rockiness[hi], snapshot.compaction[hi],
                       snapshot.waterDepth[hi] > 0.0f ? TextFormat("  water %.2f", snapshot.waterDepth[hi]) : "");
        const int labelWidth = MeasureText(tileLabel.c_str(), 16);
        DrawText(tileLabel.c_str(), viewportW - labelWidth - 10, 8, 16, cursorColor);
    }

    if (snapshot.paused) {
        const char* pausedLabel = "PAUSED";
        const int labelWidth = MeasureText(pausedLabel, 24);
        DrawRectangle(viewportW / 2 - labelWidth / 2 - 10, kHudHeight + 8, labelWidth + 20, 32, hudColor);
        DrawText(pausedLabel, viewportW / 2 - labelWidth / 2, kHudHeight + 12, 24, pausedColor);
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        return AppScreen::MainMenu;
    }
    return AppScreen::Simulation;
}

} // namespace SimulationScreen

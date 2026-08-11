#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <raylib.h>

#include "config/Config.hpp"

// Клиент — графическое приложение (raylib), не консоль. Подключается
// только по WebSocket-протоколу сервера (07_TechStack.md, п.6) — никакого
// доступа к core. Сеть и отрисовка идут в разных потоках: IXWebSocket сам
// поднимает поток для обработки сообщений, здесь — только основной цикл
// окна.

namespace {

struct WorldState {
    int areaWidth = 0;
    int areaHeight = 0;
    std::uint64_t tick = 0;
    bool paused = false;
    std::vector<std::pair<int, int>> boulders;
};

std::mutex g_stateMutex;
WorldState g_state;

void handleMessage(const std::string& payload) {
    const auto json = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return;
    }

    const std::string type = json.value("type", "");

    std::lock_guard<std::mutex> lock(g_stateMutex);

    if (type == "world_snapshot") {
        g_state.areaWidth = json["area"]["width"].get<int>();
        g_state.areaHeight = json["area"]["height"].get<int>();
        g_state.tick = json.value("tick", static_cast<std::uint64_t>(0));
        g_state.paused = json.value("paused", false);
        g_state.boulders.clear();
        for (const auto& b : json["boulders"]) {
            g_state.boulders.emplace_back(b["x"].get<int>(), b["y"].get<int>());
        }
    } else if (type == "tick") {
        g_state.tick = json.value("tick", g_state.tick);
    } else if (type == "pause_state") {
        g_state.paused = json.value("paused", g_state.paused);
    }
}

WorldState snapshotState() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_state;
}

} // namespace

int main(int argc, char** argv) {
    const std::string configPath = argc > 1 ? argv[1] : goblins::defaultConfigPathNextToExecutable();
    goblins::ensureConfigExists<goblins::ClientConfig>(configPath);
    const auto config = goblins::loadClientConfig(configPath);

    ix::initNetSystem();

    ix::WebSocket webSocket;
    webSocket.setUrl("ws://" + config.host + ":" + std::to_string(config.port));
    webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            handleMessage(msg->str);
        }
    });
    webSocket.start();

    const int tileSize = config.tile_size;
    const int viewportW = config.view.width * tileSize;
    const int viewportH = config.view.height * tileSize;
    const int hudHeight = 32;

    InitWindow(viewportW, viewportH + hudHeight, "Goblins - World Viewer");
    SetTargetFPS(60);

    const float scrollSpeedPx = static_cast<float>(config.scroll_step * tileSize);

    float viewX = 0.0f;
    float viewY = 0.0f;

    const Color backgroundColor{28, 28, 32, 255};
    const Color boulderColor{140, 128, 116, 255};
    const Color hudColor{18, 18, 20, 255};
    const Color textColor{230, 230, 230, 255};
    const Color cursorColor{255, 220, 90, 255};
    const Color pausedColor{220, 70, 70, 255};

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) viewY -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) viewY += scrollSpeedPx * dt;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) viewX -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) viewX += scrollSpeedPx * dt;

        // Пауза — не локальное состояние клиента, а запрос серверу
        // (настоящая пауза мира). Сам клиент своё "paused" не выставляет —
        // ждёт подтверждения через pause_state/world_snapshot, чтобы все
        // подключённые клиенты видели одно и то же состояние.
        if (IsKeyPressed(KEY_P)) {
            nlohmann::json request;
            request["type"] = "toggle_pause";
            webSocket.send(request.dump());
        }

        const WorldState snapshot = snapshotState();

        if (snapshot.areaWidth > 0) {
            const float maxX = std::max(0.0f, static_cast<float>(snapshot.areaWidth * tileSize - viewportW));
            const float maxY = std::max(0.0f, static_cast<float>(snapshot.areaHeight * tileSize - viewportH));
            viewX = std::clamp(viewX, 0.0f, maxX);
            viewY = std::clamp(viewY, 0.0f, maxY);
        } else {
            viewX = std::max(0.0f, viewX);
            viewY = std::max(0.0f, viewY);
        }

        // Тайл под курсором — только если мышь над картой (не над HUD) и
        // тайл существует в пределах Области.
        const Vector2 mouse = GetMousePosition();
        bool hasHoverTile = false;
        int hoverX = 0;
        int hoverY = 0;
        if (snapshot.areaWidth > 0 && mouse.x >= 0 && mouse.x < viewportW && mouse.y >= hudHeight &&
            mouse.y < hudHeight + viewportH) {
            hoverX = static_cast<int>(std::floor((mouse.x + viewX) / tileSize));
            hoverY = static_cast<int>(std::floor((mouse.y - hudHeight + viewY) / tileSize));
            hasHoverTile =
                hoverX >= 0 && hoverX < snapshot.areaWidth && hoverY >= 0 && hoverY < snapshot.areaHeight;
        }

        BeginDrawing();
        ClearBackground(backgroundColor);

        if (snapshot.areaWidth == 0) {
            const std::string waiting = "Connecting to " + config.host + ":" + std::to_string(config.port) + "...";
            DrawText(waiting.c_str(), 10, hudHeight + 10, 20, textColor);
        } else {
            for (const auto& boulder : snapshot.boulders) {
                const float screenX = static_cast<float>(boulder.first * tileSize) - viewX;
                const float screenY = static_cast<float>(boulder.second * tileSize) - viewY + hudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < hudHeight ||
                    screenY > viewportH + hudHeight) {
                    continue;
                }
                // -1px зазор между тайлами заодно рисует сетку, без
                // отдельных линий.
                DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize - 1, tileSize - 1,
                              boulderColor);
            }

            if (hasHoverTile) {
                const float screenX = static_cast<float>(hoverX * tileSize) - viewX;
                const float screenY = static_cast<float>(hoverY * tileSize) - viewY + hudHeight;
                DrawRectangleLines(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                                    cursorColor);
            }
        }

        DrawRectangle(0, 0, viewportW, hudHeight, hudColor);
        DrawText(TextFormat("Tick: %llu   Area: %dx%d   View: (%d,%d)   WASD - scroll   P - pause",
                             static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                             static_cast<int>(viewX / tileSize), static_cast<int>(viewY / tileSize)),
                 10, 8, 18, textColor);

        if (hasHoverTile) {
            const std::string tileLabel = TextFormat("Tile: (%d, %d)", hoverX, hoverY);
            const int labelWidth = MeasureText(tileLabel.c_str(), 18);
            DrawText(tileLabel.c_str(), viewportW - labelWidth - 10, 8, 18, cursorColor);
        }

        if (snapshot.paused) {
            const char* pausedLabel = "PAUSED";
            const int labelWidth = MeasureText(pausedLabel, 24);
            DrawRectangle(viewportW / 2 - labelWidth / 2 - 10, hudHeight + 8, labelWidth + 20, 32, hudColor);
            DrawText(pausedLabel, viewportW / 2 - labelWidth / 2, hudHeight + 12, 24, pausedColor);
        }

        EndDrawing();
    }

    CloseWindow();

    webSocket.stop();
    ix::uninitNetSystem();

    return 0;
}

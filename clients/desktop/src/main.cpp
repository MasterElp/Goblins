#include <algorithm>
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
        g_state.boulders.clear();
        for (const auto& b : json["boulders"]) {
            g_state.boulders.emplace_back(b["x"].get<int>(), b["y"].get<int>());
        }
    } else if (type == "tick") {
        g_state.tick = json.value("tick", g_state.tick);
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

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) viewY -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) viewY += scrollSpeedPx * dt;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) viewX -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) viewX += scrollSpeedPx * dt;

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
        }

        DrawRectangle(0, 0, viewportW, hudHeight, hudColor);
        DrawText(TextFormat("Tick: %llu   Area: %dx%d   View: (%d,%d)   WASD - scroll",
                             static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                             static_cast<int>(viewX / tileSize), static_cast<int>(viewY / tileSize)),
                 10, 8, 18, textColor);

        EndDrawing();
    }

    CloseWindow();

    webSocket.stop();
    ix::uninitNetSystem();

    return 0;
}

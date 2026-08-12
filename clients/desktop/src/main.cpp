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

#include "SettingsPanel.hpp"
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

    // Почва — плоские массивы (row-major, x + y*areaWidth), одно значение
    // на тайл. Вода — так же, глубина 0 значит "воды нет" (сам компонент
    // на сервере просто отсутствует у тайла — см. 02_CorePrinciples.md,
    // п.3: отсутствие компонента = отсутствие возможности).
    std::vector<float> moisture;
    std::vector<float> rockiness;
    std::vector<float> compaction;
    std::vector<float> waterDepth;

    // Параметры, которыми был сгенерирован текущий мир — стартовая точка
    // для панели настроек.
    goblins::RegenerationRequest generation{};
    bool hasGeneration = false;
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

        const std::size_t cellCount = static_cast<std::size_t>(g_state.areaWidth) * g_state.areaHeight;
        g_state.moisture.assign(cellCount, 0.0f);
        g_state.rockiness.assign(cellCount, 0.0f);
        g_state.compaction.assign(cellCount, 0.0f);
        g_state.waterDepth.assign(cellCount, 0.0f);

        if (json.contains("soil")) {
            const auto& soil = json["soil"];
            if (soil.contains("moisture")) {
                g_state.moisture = soil["moisture"].get<std::vector<float>>();
            }
            if (soil.contains("rockiness")) {
                g_state.rockiness = soil["rockiness"].get<std::vector<float>>();
            }
            if (soil.contains("compaction")) {
                g_state.compaction = soil["compaction"].get<std::vector<float>>();
            }
        }
        if (json.contains("water")) {
            for (const auto& w : json["water"]) {
                const int wx = w.value("x", -1);
                const int wy = w.value("y", -1);
                if (wx >= 0 && wx < g_state.areaWidth && wy >= 0 && wy < g_state.areaHeight) {
                    g_state.waterDepth[static_cast<std::size_t>(wy) * g_state.areaWidth + wx] =
                        w.value("depth", 0.0f);
                }
            }
        }

        if (json.contains("terrain")) {
            g_state.generation.terrain = json["terrain"].get<goblins::TerrainConfig>();
        }
        g_state.generation.terrain_seed = json.value("terrain_seed", g_state.generation.terrain_seed);
        g_state.generation.boulder_count = json.value("boulder_count", g_state.generation.boulder_count);
        g_state.generation.boulder_seed = json.value("boulder_seed", g_state.generation.boulder_seed);
        g_state.hasGeneration = true;
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

// Линейная интерполяция цвета.
Color lerpColor(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{static_cast<unsigned char>(a.r + (b.r - a.r) * t), static_cast<unsigned char>(a.g + (b.g - a.g) * t),
                 static_cast<unsigned char>(a.b + (b.b - a.b) * t), 255};
}

// Цвет почвы — смешение трёх параметров разными оттенками: каменистость
// и утрамбованность задают материал (серый/утоптанная земля), влажность
// затемняет поверх.
Color soilColor(float moisture, float rockiness, float compaction) {
    static const Color dirt{101, 67, 33, 255};
    static const Color rock{132, 130, 124, 255};
    static const Color packed{150, 132, 96, 255};
    static const Color wet{40, 46, 38, 255};

    Color c = lerpColor(dirt, rock, rockiness);
    c = lerpColor(c, packed, compaction * (1.0f - rockiness * 0.5f));
    c = lerpColor(c, wet, moisture * 0.6f);
    return c;
}

// Цвет воды — от мелкой (светлее, бирюзовее) до глубокой (тёмно-синяя).
Color waterColor(float depth) {
    static const Color shallow{90, 150, 175, 235};
    static const Color deep{18, 36, 66, 255};
    const float t = std::clamp(depth / 3.0f, 0.0f, 1.0f);
    return lerpColor(shallow, deep, t);
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
    const int panelWidth = 340;

    InitWindow(viewportW + panelWidth, viewportH + hudHeight, "Goblins - World Viewer");
    SetTargetFPS(60);

    const float scrollSpeedPx = static_cast<float>(config.scroll_step * tileSize);

    float viewX = 0.0f;
    float viewY = 0.0f;

    const Color backgroundColor{28, 28, 32, 255};
    const Color boulderColor{70, 66, 62, 255};
    const Color hudColor{18, 18, 20, 255};
    const Color textColor{230, 230, 230, 255};
    const Color cursorColor{255, 220, 90, 255};
    const Color pausedColor{220, 70, 70, 255};

    SettingsPanel settingsPanel;

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();

        if (IsKeyPressed(KEY_TAB)) {
            settingsPanel.visible = !settingsPanel.visible;
        }

        // Мышь над панелью — не даём WASD одновременно скроллить карту
        // (панель и так двигает свой собственный скролл под курсором).
        const Vector2 mouse = GetMousePosition();
        const bool mouseOverPanel =
            settingsPanel.visible && mouse.x >= static_cast<float>(viewportW);

        if (!mouseOverPanel) {
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) viewY -= scrollSpeedPx * dt;
            if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) viewY += scrollSpeedPx * dt;
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) viewX -= scrollSpeedPx * dt;
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) viewX += scrollSpeedPx * dt;
        }

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

        if (snapshot.hasGeneration) {
            settingsPanel.loadFrom(snapshot.generation);
        }

        if (snapshot.areaWidth > 0) {
            const float maxX = std::max(0.0f, static_cast<float>(snapshot.areaWidth * tileSize - viewportW));
            const float maxY = std::max(0.0f, static_cast<float>(snapshot.areaHeight * tileSize - viewportH));
            viewX = std::clamp(viewX, 0.0f, maxX);
            viewY = std::clamp(viewY, 0.0f, maxY);
        } else {
            viewX = std::max(0.0f, viewX);
            viewY = std::max(0.0f, viewY);
        }

        // Тайл под курсором — только если мышь над картой (не над HUD и
        // не над панелью настроек) и тайл существует в пределах Области.
        bool hasHoverTile = false;
        int hoverX = 0;
        int hoverY = 0;
        if (!mouseOverPanel && snapshot.areaWidth > 0 && mouse.x >= 0 && mouse.x < viewportW &&
            mouse.y >= hudHeight && mouse.y < hudHeight + viewportH) {
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
            BeginScissorMode(0, hudHeight, viewportW, viewportH);

            // Видимый диапазон тайлов — не рисуем то, что не попадает в
            // окно просмотра.
            const int firstTileX = std::max(0, static_cast<int>(viewX) / tileSize);
            const int firstTileY = std::max(0, static_cast<int>(viewY) / tileSize);
            const int lastTileX = std::min(snapshot.areaWidth - 1, firstTileX + viewportW / tileSize + 1);
            const int lastTileY = std::min(snapshot.areaHeight - 1, firstTileY + viewportH / tileSize + 1);

            for (int ty = firstTileY; ty <= lastTileY; ++ty) {
                for (int tx = firstTileX; tx <= lastTileX; ++tx) {
                    const std::size_t i = static_cast<std::size_t>(ty) * snapshot.areaWidth + tx;
                    const float screenX = static_cast<float>(tx * tileSize) - viewX;
                    const float screenY = static_cast<float>(ty * tileSize) - viewY + hudHeight;

                    const Color soil = soilColor(snapshot.moisture[i], snapshot.rockiness[i], snapshot.compaction[i]);
                    DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize, soil);

                    if (snapshot.waterDepth[i] > 0.0f) {
                        DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                                      waterColor(snapshot.waterDepth[i]));
                    }
                }
            }

            for (const auto& boulder : snapshot.boulders) {
                const float screenX = static_cast<float>(boulder.first * tileSize) - viewX;
                const float screenY = static_cast<float>(boulder.second * tileSize) - viewY + hudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < hudHeight ||
                    screenY > viewportH + hudHeight) {
                    continue;
                }
                DrawRectangle(static_cast<int>(screenX) + 2, static_cast<int>(screenY) + 2, tileSize - 4,
                              tileSize - 4, boulderColor);
            }

            if (hasHoverTile) {
                const float screenX = static_cast<float>(hoverX * tileSize) - viewX;
                const float screenY = static_cast<float>(hoverY * tileSize) - viewY + hudHeight;
                DrawRectangleLines(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize,
                                    cursorColor);
            }

            EndScissorMode();
        }

        DrawRectangle(0, 0, viewportW, hudHeight, hudColor);
        DrawText(TextFormat("Tick: %llu   Area: %dx%d   View: (%d,%d)   WASD-scroll  P-pause  TAB-settings",
                             static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                             static_cast<int>(viewX / tileSize), static_cast<int>(viewY / tileSize)),
                 10, 8, 16, textColor);

        if (hasHoverTile) {
            const std::size_t hi = static_cast<std::size_t>(hoverY) * snapshot.areaWidth + hoverX;
            const std::string tileLabel =
                TextFormat("Tile (%d,%d)  moist %.2f  rock %.2f  pack %.2f%s", hoverX, hoverY,
                           snapshot.moisture[hi], snapshot.rockiness[hi], snapshot.compaction[hi],
                           snapshot.waterDepth[hi] > 0.0f ? TextFormat("  water %.2f", snapshot.waterDepth[hi]) : "");
            const int labelWidth = MeasureText(tileLabel.c_str(), 16);
            DrawText(tileLabel.c_str(), viewportW - labelWidth - 10, 8, 16, cursorColor);
        }

        if (snapshot.paused) {
            const char* pausedLabel = "PAUSED";
            const int labelWidth = MeasureText(pausedLabel, 24);
            DrawRectangle(viewportW / 2 - labelWidth / 2 - 10, hudHeight + 8, labelWidth + 20, 32, hudColor);
            DrawText(pausedLabel, viewportW / 2 - labelWidth / 2, hudHeight + 12, 24, pausedColor);
        }

        // Панель настроек — справа. Возвращает true и заполняет request,
        // если пользователь нажал "Regenerate": шлём запрос серверу, сам
        // мир регенерирует он (единый источник истины), клиент только
        // просит.
        goblins::RegenerationRequest regenerateRequest;
        const Rectangle panelBounds{static_cast<float>(viewportW), 0, static_cast<float>(panelWidth),
                                     static_cast<float>(viewportH + hudHeight)};
        if (settingsPanel.draw(panelBounds, regenerateRequest)) {
            nlohmann::json request;
            request["type"] = "regenerate";
            request["params"] = regenerateRequest;
            webSocket.send(request.dump());
        }

        EndDrawing();
    }

    CloseWindow();

    webSocket.stop();
    ix::uninitNetSystem();

    return 0;
}

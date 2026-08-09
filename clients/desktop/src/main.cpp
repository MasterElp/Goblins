#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "Console.hpp"

// Клиент — чистый C++: терминальный ASCII-рендер вместо GUI-библиотеки.
// Подключается только по WebSocket-протоколу сервера (07_TechStack.md,
// п.6) — никакого доступа к core.

namespace {

struct WorldState {
    int areaWidth = 0;
    int areaHeight = 0;
    std::uint64_t tick = 0;
    std::vector<std::pair<int, int>> boulders;
};

std::mutex g_stateMutex;
WorldState g_state;
std::atomic<bool> g_dirty{true};

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
    } else {
        return;
    }

    g_dirty = true;
}

void render(int viewX, int viewY, int viewW, int viewH) {
    WorldState snapshot;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        snapshot = g_state;
    }

    goblins::consoleClear();

    std::printf("Goblins world viewer  |  Tick: %llu  |  Area: %dx%d  |  View: (%d,%d)\n",
                static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                viewX, viewY);
    std::printf("WASD - scroll, Q - quit\n\n");

    std::vector<std::string> grid(static_cast<std::size_t>(viewH), std::string(static_cast<std::size_t>(viewW), '.'));
    for (const auto& boulder : snapshot.boulders) {
        const int localX = boulder.first - viewX;
        const int localY = boulder.second - viewY;
        if (localX >= 0 && localX < viewW && localY >= 0 && localY < viewH) {
            grid[static_cast<std::size_t>(localY)][static_cast<std::size_t>(localX)] = '#';
        }
    }

    for (const auto& row : grid) {
        std::puts(row.c_str());
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "localhost";
    int port = 9002;
    if (argc > 1) {
        const std::string arg = argv[1];
        const auto colon = arg.find(':');
        if (colon != std::string::npos) {
            host = arg.substr(0, colon);
            port = std::atoi(arg.substr(colon + 1).c_str());
        } else {
            host = arg;
        }
    }

    ix::initNetSystem();

    ix::WebSocket webSocket;
    webSocket.setUrl("ws://" + host + ":" + std::to_string(port));
    webSocket.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            handleMessage(msg->str);
        }
    });
    webSocket.start();

    goblins::consoleInit();

    constexpr int viewW = 60;
    constexpr int viewH = 25;
    constexpr int step = 5;
    int viewX = 0;
    int viewY = 0;

    render(viewX, viewY, viewW, viewH);

    bool running = true;
    while (running) {
        if (goblins::consoleKeyPressed()) {
            const char key = goblins::consoleReadKey();
            bool moved = true;

            switch (key) {
                case 'w':
                case 'W':
                    viewY -= step;
                    break;
                case 's':
                case 'S':
                    viewY += step;
                    break;
                case 'a':
                case 'A':
                    viewX -= step;
                    break;
                case 'd':
                case 'D':
                    viewX += step;
                    break;
                case 'q':
                case 'Q':
                    running = false;
                    moved = false;
                    break;
                default:
                    moved = false;
                    break;
            }

            if (moved) {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                if (g_state.areaWidth > 0) {
                    viewX = std::max(0, std::min(viewX, std::max(0, g_state.areaWidth - viewW)));
                    viewY = std::max(0, std::min(viewY, std::max(0, g_state.areaHeight - viewH)));
                } else {
                    viewX = std::max(0, viewX);
                    viewY = std::max(0, viewY);
                }
                g_dirty = true;
            }
        }

        if (g_dirty.exchange(false)) {
            render(viewX, viewY, viewW, viewH);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    goblins::consoleRestore();
    webSocket.stop();
    ix::uninitNetSystem();

    return 0;
}

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "config/Config.hpp"

// Снимок состояния мира на стороне клиента — обновляется из сетевого
// потока (IXWebSocket), читается из потока рендера. Общий для экранов
// "Генерация мира" и "Симуляция": обе показывают один и тот же мир,
// просто по-разному его отображают.
struct WorldState {
    int areaWidth = 0;
    int areaHeight = 0;
    std::uint64_t tick = 0;
    bool paused = false;
    std::vector<std::pair<int, int>> boulders;

    // Почва — плоские массивы (row-major, x + y*areaWidth), одно значение
    // на тайл. Вода — так же; глубина 0 значит "воды нет" (сам компонент
    // на сервере просто отсутствует у тайла — см. 02_CorePrinciples.md,
    // п.3: отсутствие компонента = отсутствие возможности).
    std::vector<float> moisture;
    std::vector<float> rockiness;
    std::vector<float> compaction;
    std::vector<float> waterDepth;

    // Параметры, которыми сгенерирован текущий мир — стартовая точка для
    // панели настроек генерации.
    goblins::RegenerationRequest generation{};
    bool hasGeneration = false;
    bool connected = false;
};

// Единственное WebSocket-соединение на весь клиент — устанавливается
// один раз в main() и используется всеми экранами, которым нужны данные
// о мире (07_TechStack.md, п.6: клиент получает данные только по
// протоколу сервера).
class NetworkClient {
public:
    void connect(const std::string& host, int port);
    void disconnect();

    WorldState snapshot() const;

    void sendTogglePause();
    void sendRegenerate(const goblins::RegenerationRequest& request);

private:
    void handleMessage(const std::string& payload);

    ix::WebSocket webSocket_;
    mutable std::mutex mutex_;
    WorldState state_;
};

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <ixwebsocket/IXWebSocketServer.h>

#include "core/World.hpp"

namespace goblins {

// Сетевой слой сервера: транспорт (WebSocket) и сериализация (JSON) живут
// здесь, а не в core — согласно "Все взаимодействия происходят через
// интерфейсы" (02_CorePrinciples.md) и границам модулей (07_TechStack.md,
// п.6: core не знает о server, server не меняет core).
//
// Протокол (версия 2):
//   Сервер -> клиент, сразу при подключении:
//     {"type": "world_snapshot", "area": {"width", "height"}, "tick": N,
//      "paused": bool, "boulders": [{"x", "y"}, ...]}
//   Сервер -> клиент, после каждого тика:
//     {"type": "tick", "tick": N}
//   Сервер -> клиент, при изменении паузы (рассылается всем клиентам):
//     {"type": "pause_state", "paused": bool}
//   Клиент -> сервер, запрос переключить паузу:
//     {"type": "toggle_pause"}
class NetworkServer {
public:
    // paused — ссылка на флаг паузы игрового цикла (GameLoop::paused).
    // NetworkServer управляет им напрямую по запросу клиента: так у
    // состояния паузы остаётся один источник истины, а не две
    // независимые копии (в GameLoop и в NetworkServer), которые могли бы
    // разойтись.
    NetworkServer(const World& world, const std::string& host, int port, std::atomic<bool>& paused);

    // Возвращает false, если порт не удалось занять — например, он уже
    // используется другим процессом.
    bool start();
    void stop();

    // Вызывается из GameLoop::onTickComplete — рассылает номер тика всем
    // подключённым клиентам.
    void broadcastTick(std::uint64_t tick);

private:
    std::string buildSnapshotMessage() const;
    void handleClientMessage(const std::string& payload);
    void broadcastPauseState();
    void broadcastToAll(const std::string& payload);

    const World& world_;
    ix::WebSocketServer server_;
    std::atomic<bool>& paused_;
};

} // namespace goblins

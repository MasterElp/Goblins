#pragma once

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
// Протокол (версия 1, минимальный):
//   Сервер -> клиент, сразу при подключении:
//     {"type": "world_snapshot", "area": {"width", "height"}, "tick": N,
//      "boulders": [{"x", "y"}, ...]}
//   Сервер -> клиент, после каждого тика:
//     {"type": "tick", "tick": N}
class NetworkServer {
public:
    NetworkServer(const World& world, int port);

    // Возвращает false, если порт не удалось занять — например, он уже
    // используется другим процессом.
    bool start();
    void stop();

    // Вызывается из GameLoop::onTickComplete — рассылает номер тика всем
    // подключённым клиентам.
    void broadcastTick(std::uint64_t tick);

private:
    std::string buildSnapshotMessage() const;

    const World& world_;
    ix::WebSocketServer server_;
};

} // namespace goblins

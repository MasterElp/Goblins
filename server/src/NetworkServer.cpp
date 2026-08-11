#include "server/NetworkServer.hpp"

#include <iostream>

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/TimeComponent.hpp"

namespace goblins {

NetworkServer::NetworkServer(const World& world, const std::string& host, int port)
    : world_(world), server_(port, host) {
    server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*state*/,
               ix::WebSocket& webSocket,
               const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                // Новый клиент сразу получает полный снапшот мира —
                // ему ещё не известны накопленные изменения.
                webSocket.send(buildSnapshotMessage());
            }
        });
}

bool NetworkServer::start() {
    ix::initNetSystem();

    const auto result = server_.listen();
    if (!result.first) {
        std::cerr << "NetworkServer: failed to bind port: " << result.second << "\n";
        return false;
    }
    server_.start();
    return true;
}

void NetworkServer::stop() {
    server_.stop();
}

void NetworkServer::broadcastTick(std::uint64_t tick) {
    nlohmann::json message;
    message["type"] = "tick";
    message["tick"] = tick;
    const std::string payload = message.dump();

    for (const auto& client : server_.getClients()) {
        client->send(payload);
    }
}

std::string NetworkServer::buildSnapshotMessage() const {
    nlohmann::json message;
    message["type"] = "world_snapshot";
    message["area"]["width"] = world_.area().width();
    message["area"]["height"] = world_.area().height();

    const auto& time = world_.registry().get<const TimeComponent>(world_.worldEntity());
    message["tick"] = time.tick;

    auto boulders = nlohmann::json::array();
    world_.registry()
        .view<const ImpassableComponent, const PositionComponent>()
        .each([&](auto /*entity*/, const PositionComponent& pos) {
            boulders.push_back({{"x", pos.x}, {"y", pos.y}});
        });
    message["boulders"] = boulders;

    return message.dump();
}

} // namespace goblins

#include "server/NetworkServer.hpp"

#include <cmath>
#include <iostream>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "server/WorldSave.hpp"

namespace goblins {

namespace {

// Округление до 3 знаков — почве не нужна точность double-текста в JSON,
// а на карте 100x100 экономит заметную часть трафика на трёх плотных
// массивах.
float round3(float value) {
    return std::round(value * 1000.0f) / 1000.0f;
}

} // namespace

NetworkServer::NetworkServer(const World& world, const std::string& host, int port, std::atomic<bool>& paused,
                              ServerConfig baseConfig, std::string configPath,
                              std::filesystem::path savesDirectory)
    : world_(world), server_(port, host), paused_(paused), baseConfig_(std::move(baseConfig)),
      configPath_(std::move(configPath)), savesDirectory_(std::move(savesDirectory)) {
    server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*state*/,
               ix::WebSocket& webSocket,
               const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                // Новый клиент сразу получает полный снапшот мира —
                // ему ещё не известны накопленные изменения — и список
                // сохранённых миров: с него начинается главное меню.
                webSocket.send(buildSnapshotMessage());
                webSocket.send(buildWorldListMessage());
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                handleClientMessage(msg->str);
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
    broadcastToAll(message.dump());
}

void NetworkServer::broadcastSnapshot() {
    broadcastToAll(buildSnapshotMessage());
}

void NetworkServer::setCurrentGenerationConfig(const RegenerationRequest& config) {
    std::lock_guard<std::mutex> lock(generationConfigMutex_);
    currentGenerationConfig_ = config;
}

RegenerationRequest NetworkServer::currentGenerationConfig() const {
    std::lock_guard<std::mutex> lock(generationConfigMutex_);
    return currentGenerationConfig_;
}

void NetworkServer::setCurrentWorldName(const std::string& name) {
    std::lock_guard<std::mutex> lock(generationConfigMutex_);
    currentWorldName_ = name;
}

std::string NetworkServer::currentWorldName() const {
    std::lock_guard<std::mutex> lock(generationConfigMutex_);
    return currentWorldName_;
}

std::optional<RegenerationRequest> NetworkServer::takePendingRegeneration() {
    std::lock_guard<std::mutex> lock(pendingRegenerationMutex_);
    auto result = pendingRegeneration_;
    pendingRegeneration_.reset();
    return result;
}

std::optional<StartSimulationRequest> NetworkServer::takePendingStartSimulation() {
    std::lock_guard<std::mutex> lock(pendingStartMutex_);
    auto result = pendingStart_;
    pendingStart_.reset();
    return result;
}

std::optional<SaveWorldRequest> NetworkServer::takePendingSaveWorld() {
    std::lock_guard<std::mutex> lock(pendingSaveWorldMutex_);
    auto result = pendingSaveWorld_;
    pendingSaveWorld_.reset();
    return result;
}

void NetworkServer::handleClientMessage(const std::string& payload) {
    // Этот колбэк вызывается на внутреннем потоке IXWebSocket, не на
    // потоке GameLoop::run(). Прямая мутация ECS registry отсюда была бы
    // гонкой данных — поэтому регенерация только складывается в
    // pendingRegeneration_ (под мьютексом), а выполняется позже, на
    // потоке GameLoop (см. takePendingRegeneration). paused_ — атомарный,
    // его трогать отсюда безопасно напрямую.
    const auto json = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return;
    }

    const std::string type = json.value("type", "");
    if (type == "toggle_pause") {
        const bool newState = !paused_.load();
        pauseCommandCount_.fetch_add(1);
        paused_.store(newState);
        std::cout << "World " << (newState ? "paused" : "resumed") << " by client request.\n";
        broadcastPauseState();
    } else if (type == "regenerate") {
        if (!json.contains("params")) {
            return;
        }
        try {
            RegenerationRequest request = json.at("params").get<RegenerationRequest>();
            std::lock_guard<std::mutex> lock(pendingRegenerationMutex_);
            pendingRegeneration_ = request;
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "NetworkServer: invalid regenerate request (" << e.what() << ")\n";
        }
    } else if (type == "start_simulation") {
        StartSimulationRequest request;
        // Имя мира приходит от клиента и превращается в путь на диске —
        // проверяем его здесь, до постановки в очередь, чтобы на потоке
        // GameLoop уже не разбираться с заведомо негодным запросом.
        request.worldName = json.value("world", std::string{});
        if (!request.worldName.empty() && !isValidWorldName(request.worldName)) {
            std::cerr << "NetworkServer: rejected start_simulation with invalid world name.\n";
            broadcastNotice("error", "Invalid world name.");
            return;
        }
        std::lock_guard<std::mutex> lock(pendingStartMutex_);
        pendingStart_ = request;
    } else if (type == "list_worlds") {
        // Только чтение каталога сохранений, ECS registry не трогаем —
        // можно прямо здесь, на сетевом потоке (как save_generation_config).
        broadcastWorldList();
    } else if (type == "save_world") {
        SaveWorldRequest request;
        request.name = json.value("name", std::string{});
        if (!request.name.empty() && !isValidWorldName(request.name)) {
            std::cerr << "NetworkServer: rejected save_world with invalid world name.\n";
            broadcastNotice("error", "Invalid world name.");
            return;
        }
        std::lock_guard<std::mutex> lock(pendingSaveWorldMutex_);
        pendingSaveWorld_ = request;
    } else if (type == "save_generation_config") {
        // Только файловый ввод-вывод, ECS registry не трогаем — можно
        // прямо здесь, на сетевом потоке, как и toggle_pause.
        ServerConfig toSave = baseConfig_;
        {
            std::lock_guard<std::mutex> lock(generationConfigMutex_);
            toSave.terrain_seed = currentGenerationConfig_.terrain_seed;
            toSave.terrain = currentGenerationConfig_.terrain;
            toSave.boulder_count = currentGenerationConfig_.boulder_count;
            toSave.boulder_seed = currentGenerationConfig_.boulder_seed;
        }
        saveServerConfig(configPath_, toSave);
        std::cout << "Generation config saved to '" << configPath_ << "'.\n";
    } else if (type == "stop_simulation") {
        // В отличие от toggle_pause, это не переключение, а безусловная
        // остановка — клиент нажал "Back", повторный запрос не должен
        // случайно снова запустить луп. paused_ атомарный, трогать его
        // отсюда (сетевой поток) безопасно, как и в toggle_pause.
        pauseCommandCount_.fetch_add(1);
        paused_.store(true);
        std::cout << "Simulation stopped by client request.\n";
        broadcastPauseState();
    }
}

void NetworkServer::broadcastPauseState() {
    nlohmann::json message;
    message["type"] = "pause_state";
    message["paused"] = paused_.load();
    broadcastToAll(message.dump());
}

std::uint64_t NetworkServer::pauseCommandCount() const {
    return pauseCommandCount_.load();
}

void NetworkServer::broadcastWorldList() {
    broadcastToAll(buildWorldListMessage());
}

void NetworkServer::broadcastNotice(const std::string& level, const std::string& text) {
    nlohmann::json message;
    message["type"] = "notice";
    message["level"] = level;
    message["text"] = text;
    broadcastToAll(message.dump());
}

std::string NetworkServer::buildWorldListMessage() const {
    nlohmann::json message;
    message["type"] = "world_list";
    message["worlds"] = listWorldSaves(savesDirectory_);
    {
        std::lock_guard<std::mutex> lock(generationConfigMutex_);
        message["current"] = currentWorldName_;
    }
    return message.dump();
}

void NetworkServer::broadcastToAll(const std::string& payload) {
    for (const auto& client : server_.getClients()) {
        client->send(payload);
    }
}

std::string NetworkServer::buildSnapshotMessage() const {
    nlohmann::json message;
    message["type"] = "world_snapshot";

    const int width = world_.area().width();
    const int height = world_.area().height();
    message["area"]["width"] = width;
    message["area"]["height"] = height;
    message["paused"] = paused_.load();

    const auto& time = world_.registry().get<const TimeComponent>(world_.worldEntity());
    message["tick"] = time.tick;

    {
        std::lock_guard<std::mutex> lock(generationConfigMutex_);
        message["world"] = currentWorldName_;
        message["terrain_seed"] = currentGenerationConfig_.terrain_seed;
        message["terrain"] = currentGenerationConfig_.terrain;
        message["boulder_count"] = currentGenerationConfig_.boulder_count;
        message["boulder_seed"] = currentGenerationConfig_.boulder_seed;
    }

    auto boulders = nlohmann::json::array();
    world_.registry()
        .view<const ImpassableComponent, const PositionComponent>()
        .each([&](auto /*entity*/, const PositionComponent& pos) {
            boulders.push_back({{"x", pos.x}, {"y", pos.y}});
        });
    message["boulders"] = boulders;

    // Почва — плоские массивы по всей Области (row-major), компактнее,
    // чем объект на тайл при 10000+ тайлах.
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> moisture(cellCount, 0.0f);
    std::vector<float> rockiness(cellCount, 0.0f);
    std::vector<float> compaction(cellCount, 0.0f);

    world_.registry()
        .view<const PositionComponent, const SoilComponent>()
        .each([&](const PositionComponent& pos, const SoilComponent& soil) {
            const std::size_t i = static_cast<std::size_t>(pos.y) * width + pos.x;
            moisture[i] = round3(soil.moisture);
            rockiness[i] = round3(soil.rockiness);
            compaction[i] = round3(soil.compaction);
        });

    message["soil"]["moisture"] = moisture;
    message["soil"]["rockiness"] = rockiness;
    message["soil"]["compaction"] = compaction;

    // Вода — только тайлы, где она реально есть (03_CorePrinciples.md,
    // п.3: отсутствие компонента = отсутствие возможности); в разреженном
    // виде дешевле, чем ещё один плотный массив на всю Область.
    auto water = nlohmann::json::array();
    world_.registry()
        .view<const PositionComponent, const WaterComponent>()
        .each([&](const PositionComponent& pos, const WaterComponent& w) {
            water.push_back({{"x", pos.x}, {"y", pos.y}, {"depth", round3(w.depth)}});
        });
    message["water"] = water;

    return message.dump();
}

} // namespace goblins

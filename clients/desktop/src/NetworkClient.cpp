#include "NetworkClient.hpp"

#include <nlohmann/json.hpp>

void NetworkClient::connect(const std::string& host, int port) {
    webSocket_.setUrl("ws://" + host + ":" + std::to_string(port));
    webSocket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            handleMessage(msg->str);
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.connected = true;
        } else if (msg->type == ix::WebSocketMessageType::Close ||
                   msg->type == ix::WebSocketMessageType::Error) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.connected = false;
        }
    });
    webSocket_.start();
}

void NetworkClient::disconnect() {
    webSocket_.stop();
}

WorldState NetworkClient::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void NetworkClient::sendTogglePause() {
    nlohmann::json request;
    request["type"] = "toggle_pause";
    webSocket_.send(request.dump());
}

void NetworkClient::sendRegenerate(const goblins::RegenerationRequest& request) {
    nlohmann::json message;
    message["type"] = "regenerate";
    message["params"] = request;
    webSocket_.send(message.dump());
}

void NetworkClient::sendStartSimulation() {
    nlohmann::json request;
    request["type"] = "start_simulation";
    webSocket_.send(request.dump());
}

void NetworkClient::sendStopSimulation() {
    nlohmann::json request;
    request["type"] = "stop_simulation";
    webSocket_.send(request.dump());
}

void NetworkClient::sendSaveGenerationConfig() {
    nlohmann::json request;
    request["type"] = "save_generation_config";
    webSocket_.send(request.dump());
}

void NetworkClient::handleMessage(const std::string& payload) {
    const auto json = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return;
    }

    const std::string type = json.value("type", "");

    std::lock_guard<std::mutex> lock(mutex_);

    if (type == "world_snapshot") {
        state_.areaWidth = json["area"]["width"].get<int>();
        state_.areaHeight = json["area"]["height"].get<int>();
        state_.tick = json.value("tick", static_cast<std::uint64_t>(0));
        state_.paused = json.value("paused", false);

        state_.boulders.clear();
        for (const auto& b : json["boulders"]) {
            state_.boulders.emplace_back(b["x"].get<int>(), b["y"].get<int>());
        }

        const std::size_t cellCount = static_cast<std::size_t>(state_.areaWidth) * state_.areaHeight;
        state_.moisture.assign(cellCount, 0.0f);
        state_.rockiness.assign(cellCount, 0.0f);
        state_.compaction.assign(cellCount, 0.0f);
        state_.waterDepth.assign(cellCount, 0.0f);

        if (json.contains("soil")) {
            const auto& soil = json["soil"];
            if (soil.contains("moisture")) {
                state_.moisture = soil["moisture"].get<std::vector<float>>();
            }
            if (soil.contains("rockiness")) {
                state_.rockiness = soil["rockiness"].get<std::vector<float>>();
            }
            if (soil.contains("compaction")) {
                state_.compaction = soil["compaction"].get<std::vector<float>>();
            }
        }
        if (json.contains("water")) {
            for (const auto& w : json["water"]) {
                const int wx = w.value("x", -1);
                const int wy = w.value("y", -1);
                if (wx >= 0 && wx < state_.areaWidth && wy >= 0 && wy < state_.areaHeight) {
                    state_.waterDepth[static_cast<std::size_t>(wy) * state_.areaWidth + wx] = w.value("depth", 0.0f);
                }
            }
        }

        if (json.contains("terrain")) {
            state_.generation.terrain = json["terrain"].get<goblins::TerrainConfig>();
        }
        state_.generation.terrain_seed = json.value("terrain_seed", state_.generation.terrain_seed);
        state_.generation.boulder_count = json.value("boulder_count", state_.generation.boulder_count);
        state_.generation.boulder_seed = json.value("boulder_seed", state_.generation.boulder_seed);
        state_.hasGeneration = true;
    } else if (type == "tick") {
        state_.tick = json.value("tick", state_.tick);
    } else if (type == "pause_state") {
        state_.paused = json.value("paused", state_.paused);
    }
}

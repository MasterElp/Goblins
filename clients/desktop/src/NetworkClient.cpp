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

void NetworkClient::sendStartSimulation(const std::string& worldName) {
    nlohmann::json request;
    request["type"] = "start_simulation";
    request["world"] = worldName;
    webSocket_.send(request.dump());
}

void NetworkClient::sendListWorlds() {
    nlohmann::json request;
    request["type"] = "list_worlds";
    webSocket_.send(request.dump());
}

void NetworkClient::sendSaveWorld(const std::string& name) {
    nlohmann::json request;
    request["type"] = "save_world";
    request["name"] = name;
    webSocket_.send(request.dump());
}

void NetworkClient::sendDeleteWorld(const std::string& name) {
    nlohmann::json request;
    request["type"] = "delete_world";
    request["name"] = name;
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

        state_.waterSources.clear();
        if (json.contains("water_sources")) {
            for (const auto& s : json["water_sources"]) {
                state_.waterSources.emplace_back(s["x"].get<int>(), s["y"].get<int>());
            }
        }

        const std::size_t cellCount = static_cast<std::size_t>(state_.areaWidth) * state_.areaHeight;
        state_.moisture.assign(cellCount, 0.0f);
        state_.rockiness.assign(cellCount, 0.0f);
        state_.compaction.assign(cellCount, 0.0f);
        state_.minerals.assign(cellCount, 0);
        state_.waterDepth.assign(cellCount, 0.0f);
        state_.height.assign(cellCount, 0.0f);

        if (json.contains("height")) {
            state_.height = json["height"].get<std::vector<float>>();
        }

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
            if (soil.contains("minerals")) {
                state_.minerals = soil["minerals"].get<std::vector<int>>();
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

        // Растения: плотные массивы (-1 — пустая клетка), перегной —
        // разреженный список, как вода.
        state_.plantSpeciesAt.assign(cellCount, -1);
        state_.plantGrowth.assign(cellCount, 0.0f);
        state_.humus.assign(cellCount, 0);
        if (json.contains("plants")) {
            const auto& plants = json["plants"];
            if (plants.contains("species")) {
                state_.plantSpeciesAt = plants["species"].get<std::vector<int>>();
            }
            if (plants.contains("growth")) {
                // Развитость приходит целыми процентами (так плотный
                // массив в JSON вчетверо компактнее) — внутри клиента
                // удобнее долей 0..1, как и остальные слои.
                const auto percent = plants["growth"].get<std::vector<int>>();
                state_.plantGrowth.assign(percent.size(), 0.0f);
                for (std::size_t i = 0; i < percent.size(); ++i) {
                    state_.plantGrowth[i] = static_cast<float>(percent[i]) / 100.0f;
                }
            }
        }
        if (json.contains("humus")) {
            for (const auto& h : json["humus"]) {
                const int hx = h.value("x", -1);
                const int hy = h.value("y", -1);
                if (hx >= 0 && hx < state_.areaWidth && hy >= 0 && hy < state_.areaHeight) {
                    state_.humus[static_cast<std::size_t>(hy) * state_.areaWidth + hx] = h.value("minerals", 0);
                }
            }
        }
        if (json.contains("plant_species")) {
            state_.plantSpecies.clear();
            for (const auto& archetype : json["plant_species"]) {
                if (!archetype.is_object()) {
                    continue;
                }
                std::vector<std::pair<std::string, float>> traits;
                for (const auto& [name, value] : archetype.items()) {
                    if (name != "species" && value.is_number()) {
                        traits.emplace_back(name, value.get<float>());
                    }
                }
                state_.plantSpecies.push_back(std::move(traits));
            }
        }

        if (json.contains("terrain")) {
            state_.generation.terrain = json["terrain"].get<goblins::TerrainConfig>();
        }
        state_.generation.terrain_seed = json.value("terrain_seed", state_.generation.terrain_seed);
        state_.generation.boulder_count = json.value("boulder_count", state_.generation.boulder_count);
        state_.generation.boulder_seed = json.value("boulder_seed", state_.generation.boulder_seed);
        state_.hasGeneration = true;
        state_.currentWorld = json.value("world", state_.currentWorld);
    } else if (type == "pause_state") {
        state_.paused = json.value("paused", state_.paused);
    } else if (type == "world_list") {
        state_.worlds.clear();
        if (json.contains("worlds")) {
            try {
                state_.worlds = json["worlds"].get<std::vector<goblins::WorldSaveInfo>>();
            } catch (const nlohmann::json::exception&) {
                // Битый список — не повод ронять клиента: экран выбора
                // мира просто покажет, что миров нет.
            }
        }
        state_.currentWorld = json.value("current", state_.currentWorld);
        state_.worldsReceived = true;
    } else if (type == "notice") {
        state_.notice = json.value("text", std::string{});
        state_.noticeIsError = json.value("level", std::string{}) == "error";
        state_.noticeAt = std::chrono::steady_clock::now();
    }
}

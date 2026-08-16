#include "NetworkClient.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace {

// Слой в дельте — плоский массив пар "индекс тайла, новое значение"
// (см. протокол в server/NetworkServer.hpp). Индекс проверяется:
// сообщение приходит извне, и битые данные не должны приводить к записи
// за границы массива.
template <typename T, typename Decode>
void applyChangedCells(const nlohmann::json& message, const char* key, std::vector<T>& target, Decode decode) {
    if (!message.contains(key)) {
        return;
    }
    const auto& pairs = message[key];
    if (!pairs.is_array()) {
        return;
    }
    for (std::size_t p = 0; p + 1 < pairs.size(); p += 2) {
        const auto index = pairs[p].get<std::size_t>();
        if (index < target.size()) {
            target[index] = decode(pairs[p + 1].get<int>());
        }
    }
}

// Плотный слой из world_init: сервер шлёт его целыми (в JSON это вчетверо
// компактнее, чем double), клиент держит долями — так его читают
// TileColors и подписи под курсором.
std::vector<float> decodeScaled(const nlohmann::json& layers, const char* key, std::size_t cellCount, float scale) {
    std::vector<float> result(cellCount, 0.0f);
    if (!layers.contains(key)) {
        return result;
    }
    const auto raw = layers[key].get<std::vector<int>>();
    const std::size_t count = std::min(cellCount, raw.size());
    for (std::size_t i = 0; i < count; ++i) {
        result[i] = static_cast<float>(raw[i]) * scale;
    }
    return result;
}

std::vector<int> decodeInts(const nlohmann::json& layers, const char* key, std::size_t cellCount, int fallback) {
    std::vector<int> result(cellCount, fallback);
    if (!layers.contains(key)) {
        return result;
    }
    const auto raw = layers[key].get<std::vector<int>>();
    const std::size_t count = std::min(cellCount, raw.size());
    for (std::size_t i = 0; i < count; ++i) {
        result[i] = raw[i];
    }
    return result;
}

} // namespace

void NetworkClient::connect(const std::string& host, int port) {
    webSocket_.setUrl("ws://" + host + ":" + std::to_string(port));
    webSocket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            handleMessage(msg->str);
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            working_.connected = true;
            publishState();
        } else if (msg->type == ix::WebSocketMessageType::Close ||
                   msg->type == ix::WebSocketMessageType::Error) {
            working_.connected = false;
            publishState();
        }
    });
    webSocket_.start();
}

void NetworkClient::disconnect() {
    webSocket_.stop();
}

std::shared_ptr<const WorldState> NetworkClient::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

void NetworkClient::publishState() {
    ++working_.version;
    auto copy = std::make_shared<const WorldState>(working_);
    std::lock_guard<std::mutex> lock(mutex_);
    published_ = std::move(copy);
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

void NetworkClient::sendSaveGenerationConfig(const goblins::RegenerationRequest& request) {
    nlohmann::json message;
    message["type"] = "save_generation_config";
    message["params"] = request;
    webSocket_.send(message.dump());
}

// Список животных — один и тот же в world_init и в дельте, поэтому и
// разбирается одним местом. Отсутствие ключа означает "стадо не менялось",
// а не "животных не стало": пустой список сервер шлёт явным пустым
// массивом.
void NetworkClient::applyAnimals(const nlohmann::json& message) {
    if (!message.contains("animals") || !message["animals"].is_array()) {
        return;
    }
    working_.animals.clear();
    for (const auto& animal : message["animals"]) {
        if (!animal.is_object()) {
            continue;
        }
        WorldState::Animal parsed;
        parsed.x = animal.value("x", 0);
        parsed.y = animal.value("y", 0);
        parsed.species = animal.value("species", 0);
        parsed.growth = animal.value("growth", 0) * 0.01f;
        parsed.health = animal.value("health", 100) * 0.01f;
        parsed.predator = animal.value("kind", std::string{}) == "predator";
        parsed.sex = animal.value("sex", std::string{});
        parsed.desire = animal.value("desire", std::string{});
        working_.animals.push_back(std::move(parsed));
    }
}

void NetworkClient::handleMessage(const std::string& payload) {
    const auto json = nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return;
    }

    const std::string type = json.value("type", "");

    if (type == "world_init") {
        // Мир целиком: приходит при подключении и после каждой
        // регенерации/загрузки. Всё, что было накоплено дельтами до
        // этого, относится к другому миру и заменяется, а не дополняется.
        working_.areaWidth = json["area"]["width"].get<int>();
        working_.areaHeight = json["area"]["height"].get<int>();
        working_.tick = json.value("tick", static_cast<std::uint64_t>(0));
        working_.paused = json.value("paused", false);

        const int scale = json.value("scale", 1000);
        milliScale_ = scale > 0 ? 1.0f / static_cast<float>(scale) : 0.001f;

        working_.boulders.clear();
        if (json.contains("boulders")) {
            for (const auto& b : json["boulders"]) {
                working_.boulders.emplace_back(b["x"].get<int>(), b["y"].get<int>());
            }
        }

        working_.waterSources.clear();
        if (json.contains("water_sources")) {
            for (const auto& s : json["water_sources"]) {
                working_.waterSources.emplace_back(s["x"].get<int>(), s["y"].get<int>());
            }
        }

        const std::size_t cellCount = static_cast<std::size_t>(working_.areaWidth) * working_.areaHeight;
        const nlohmann::json empty = nlohmann::json::object();
        const auto& layers = json.contains("layers") ? json["layers"] : empty;

        working_.moisture = decodeScaled(layers, "moisture", cellCount, milliScale_);
        working_.rockiness = decodeScaled(layers, "rockiness", cellCount, milliScale_);
        working_.compaction = decodeScaled(layers, "compaction", cellCount, milliScale_);
        working_.height = decodeScaled(layers, "height", cellCount, milliScale_);
        working_.waterDepth = decodeScaled(layers, "water", cellCount, milliScale_);
        working_.minerals = decodeInts(layers, "minerals", cellCount, 0);
        working_.humus = decodeInts(layers, "humus", cellCount, 0);
        // -1 — пустая клетка (растение это Entity, и его отсутствие в
        // плотном массиве выражается значением-заглушкой).
        working_.plantSpeciesAt = decodeInts(layers, "species", cellCount, -1);
        // Развитость приходит целыми процентами — внутри клиента удобнее
        // долей 0..1, как и остальные слои.
        working_.plantGrowth = decodeScaled(layers, "growth", cellCount, 0.01f);
        working_.carcass = decodeScaled(layers, "carcass", cellCount, milliScale_);

        if (json.contains("plant_species")) {
            working_.plantSpecies.clear();
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
                working_.plantSpecies.push_back(std::move(traits));
            }
        }

        // Виды животных и само поголовье — тем же способом, что и трава
        // выше: клиент не знает состава генома и не должен.
        auto readSpecies = [](const nlohmann::json& list) {
            std::vector<std::vector<std::pair<std::string, float>>> species;
            for (const auto& archetype : list) {
                if (!archetype.is_object()) {
                    continue;
                }
                std::vector<std::pair<std::string, float>> traits;
                for (const auto& [name, value] : archetype.items()) {
                    if (name != "species" && value.is_number()) {
                        traits.emplace_back(name, value.get<float>());
                    }
                }
                species.push_back(std::move(traits));
            }
            return species;
        };
        if (json.contains("animal_species") && json["animal_species"].is_object()) {
            const auto& lists = json["animal_species"];
            if (lists.contains("herbivores")) {
                working_.herbivoreSpecies = readSpecies(lists["herbivores"]);
            }
            if (lists.contains("predators")) {
                working_.predatorSpecies = readSpecies(lists["predators"]);
            }
        }
        applyAnimals(json);

        // Параметры генерации — одним объектом (см. протокол в
        // server/NetworkServer.hpp): панель настроек строится из него
        // целиком.
        if (json.contains("generation")) {
            try {
                working_.generation = json["generation"].get<goblins::RegenerationRequest>();
                working_.hasGeneration = true;
            } catch (const nlohmann::json::exception&) {
                // Битые параметры — не повод не показать мир: панель
                // останется с прежними значениями.
            }
        }

        // Константы ядра — никогда не меняются, но приходят с каждым
        // world_init: перечитываем целиком, это дешевле и честнее, чем
        // проверять, не изменились ли они.
        if (json.contains("constants") && json["constants"].is_array()) {
            working_.constants.clear();
            for (const auto& constant : json["constants"]) {
                if (!constant.is_object()) {
                    continue;
                }
                working_.constants.push_back({constant.value("group", std::string{}),
                                              constant.value("name", std::string{}),
                                              constant.value("value", 0.0f)});
            }
        }
        working_.currentWorld = json.value("world", working_.currentWorld);
        publishState();
    } else if (type == "world_delta") {
        // Только изменившиеся клетки — накладываются на то, что уже
        // накоплено. Каменистость, булыжники, источники и виды травы сюда
        // не входят: они меняются лишь регенерацией, а она присылает
        // новый world_init.
        working_.tick = json.value("tick", working_.tick);
        working_.paused = json.value("paused", working_.paused);

        const float scale = milliScale_;
        const auto toFraction = [scale](int raw) { return static_cast<float>(raw) * scale; };
        applyChangedCells(json, "moisture", working_.moisture, toFraction);
        applyChangedCells(json, "compaction", working_.compaction, toFraction);
        applyChangedCells(json, "height", working_.height, toFraction);
        applyChangedCells(json, "water", working_.waterDepth, toFraction);
        applyChangedCells(json, "growth", working_.plantGrowth, [](int raw) { return raw * 0.01f; });
        applyChangedCells(json, "minerals", working_.minerals, [](int raw) { return raw; });
        applyChangedCells(json, "humus", working_.humus, [](int raw) { return raw; });
        applyChangedCells(json, "carcass", working_.carcass, toFraction);
        applyChangedCells(json, "species", working_.plantSpeciesAt, [](int raw) { return raw; });
        // Поголовье приходит целиком и только когда сдвинулось (см.
        // протокол в server/NetworkServer.hpp), поэтому не накладывается по
        // клеткам, а заменяет прежний список.
        applyAnimals(json);
        publishState();
    } else if (type == "pause_state") {
        working_.paused = json.value("paused", working_.paused);
        publishState();
    } else if (type == "world_list") {
        working_.worlds.clear();
        if (json.contains("worlds")) {
            try {
                working_.worlds = json["worlds"].get<std::vector<goblins::WorldSaveInfo>>();
            } catch (const nlohmann::json::exception&) {
                // Битый список — не повод ронять клиента: экран выбора
                // мира просто покажет, что миров нет.
            }
        }
        working_.currentWorld = json.value("current", working_.currentWorld);
        working_.worldsReceived = true;
        publishState();
    } else if (type == "notice") {
        working_.notice = json.value("text", std::string{});
        working_.noticeIsError = json.value("level", std::string{}) == "error";
        working_.noticeAt = std::chrono::steady_clock::now();
        publishState();
    }
}

#include "NetworkClient.hpp"

#include <algorithm>
#include <iterator>

#include <nlohmann/json.hpp>

namespace {

// Плотные слои приходят сотыми — и доли, и глубины (см. точность показа в
// shared/protocol/WirePrecision.hpp). Клиенту для смешения цветов удобна
// доля 0..1, и делитель у неё один на все слои: точность показа выбрана
// одна на всех. Прежде делитель приезжал в поле "scale" каждого
// world_init; теперь он постоянный, и присылать нечего.
constexpr float kFromHundredths = 0.01f;

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

// Карточка животного — всё, что о нём знает клиент. Одна на полный список
// из world_init и на родившихся в дельте: приходят они одинаковыми, и
// разбираться должны одним местом.
WorldState::Animal parseAnimal(const nlohmann::json& animal) {
    WorldState::Animal parsed;
    parsed.id = animal.value("id", static_cast<std::uint64_t>(0));
    parsed.x = animal.value("x", 0);
    parsed.y = animal.value("y", 0);
    parsed.species = animal.value("species", 0);
    parsed.growth = animal.value("growth", 0) * kFromHundredths;
    // Умолчание — целое здоровье: животное без поля "health" в сообщении
    // рисуется здоровым, а не мёртвым. Сотые, как и всё остальное в слоях.
    parsed.health = animal.value("health", 100) * kFromHundredths;
    parsed.predator = animal.value("kind", std::string{}) == "predator";
    parsed.sex = animal.value("sex", std::string{});
    parsed.desire = animal.value("desire", std::string{});
    return parsed;
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

// Массив целых из сообщения сервера: точки летописи приходят массивами
// (см. протокол в server/NetworkServer.hpp). Данные извне — нечисло на
// месте счётчика не должно ронять разбор всего сообщения.
std::vector<int> decodeIntArray(const nlohmann::json& json) {
    std::vector<int> result;
    if (!json.is_array()) {
        return result;
    }
    result.reserve(json.size());
    for (const auto& value : json) {
        result.push_back(value.is_number() ? value.get<int>() : 0);
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

void NetworkClient::sendUpdatesEnabled(bool enabled) {
    if (updatesEnabled_ == enabled) {
        return;
    }
    updatesEnabled_ = enabled;
    nlohmann::json request;
    request["type"] = "updates";
    request["enabled"] = enabled;
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

void NetworkClient::sendWatch(const std::string& kind, std::uint64_t id, int x, int y) {
    nlohmann::json request;
    request["type"] = "watch";
    request["kind"] = kind;
    request["id"] = id;
    request["x"] = x;
    request["y"] = y;
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

// Полный список животных — в world_init. Отсутствие ключа означает "стадо
// не менялось", а не "животных не стало": пустой список сервер шлёт явным
// пустым массивом.
void NetworkClient::applyAnimals(const nlohmann::json& message) {
    if (!message.contains("animals") || !message["animals"].is_array()) {
        return;
    }
    working_.animals.clear();
    for (const auto& animal : message["animals"]) {
        if (!animal.is_object()) {
            continue;
        }
        working_.animals.push_back(parseAnimal(animal));
    }
}

// Изменения списка животных (см. "animals" в описании дельты в
// server/NetworkServer.hpp). Индексы во всех частях сообщения — места в
// прежнем списке, поэтому порядок применения обязателен: сперва правки,
// потом удаления, и только потом вставка родившихся.
void NetworkClient::applyAnimalChanges(const nlohmann::json& message) {
    if (!message.contains("animals") || !message["animals"].is_object()) {
        return;
    }
    const auto& changes = message["animals"];
    auto& animals = working_.animals;

    // Пары "индекс - значение", как у тайловых слоёв, только правится не
    // клетка, а животное. Индекс проверяется: сообщение приходит извне.
    const auto applyPairs = [&](const char* key, auto&& write) {
        if (!changes.contains(key) || !changes[key].is_array()) {
            return;
        }
        const auto& pairs = changes[key];
        for (std::size_t p = 0; p + 1 < pairs.size(); p += 2) {
            const auto index = pairs[p].get<std::size_t>();
            if (index < animals.size()) {
                write(animals[index], pairs[p + 1]);
            }
        }
    };

    if (changes.contains("pos") && changes["pos"].is_array()) {
        const auto& triples = changes["pos"];
        for (std::size_t p = 0; p + 2 < triples.size(); p += 3) {
            const auto index = triples[p].get<std::size_t>();
            if (index < animals.size()) {
                animals[index].x = triples[p + 1].get<int>();
                animals[index].y = triples[p + 2].get<int>();
            }
        }
    }
    applyPairs("growth", [](WorldState::Animal& a, const nlohmann::json& v) {
        a.growth = v.get<int>() * kFromHundredths;
    });
    applyPairs("health", [](WorldState::Animal& a, const nlohmann::json& v) {
        a.health = v.get<int>() * kFromHundredths;
    });
    applyPairs("desire", [](WorldState::Animal& a, const nlohmann::json& v) {
        if (v.is_string()) {
            a.desire = v.get<std::string>();
        }
    });

    if (changes.contains("gone") && changes["gone"].is_array()) {
        // Пометить и выбросить одним проходом: удалять по одному значило бы
        // сдвигать хвост списка на каждом ушедшем, а индексы в сообщении
        // считаны в списке до всяких удалений.
        std::vector<bool> gone(animals.size(), false);
        for (const auto& index : changes["gone"]) {
            const auto i = index.get<std::size_t>();
            if (i < gone.size()) {
                gone[i] = true;
            }
        }
        std::vector<WorldState::Animal> kept;
        kept.reserve(animals.size());
        for (std::size_t i = 0; i < animals.size(); ++i) {
            if (!gone[i]) {
                kept.push_back(std::move(animals[i]));
            }
        }
        animals = std::move(kept);
    }

    if (changes.contains("born") && changes["born"].is_array()) {
        // Родившиеся приходят полными карточками и в порядке id, остаток
        // списка в том же порядке — значит слияние, а не сортировка.
        std::vector<WorldState::Animal> born;
        for (const auto& record : changes["born"]) {
            if (record.is_object()) {
                born.push_back(parseAnimal(record));
            }
        }
        std::vector<WorldState::Animal> merged;
        merged.reserve(animals.size() + born.size());
        std::merge(std::make_move_iterator(animals.begin()), std::make_move_iterator(animals.end()),
                   std::make_move_iterator(born.begin()), std::make_move_iterator(born.end()),
                   std::back_inserter(merged),
                   [](const WorldState::Animal& a, const WorldState::Animal& b) { return a.id < b.id; });
        animals = std::move(merged);
    }
}

// Летопись численности — общий разбор для world_init и дельты (см.
// протокол в server/NetworkServer.hpp). Отсутствие ключа означает "новых
// точек нет", а не "летописи не стало": заменить её целиком сервер просит
// явным "full".
void NetworkClient::applyPopulationHistory(const nlohmann::json& message, bool replace) {
    if (replace) {
        working_.populationHistory.clear();
        working_.populationInterval = 0;
    }
    if (!message.contains("history") || !message["history"].is_object()) {
        return;
    }
    const auto& history = message["history"];

    // "full" — сервер прорядил летопись (изменились все точки разом, а не
    // добавились новые) и прислал её целиком.
    if (!replace && history.value("full", false)) {
        working_.populationHistory.clear();
    }
    working_.populationInterval = history.value("interval", working_.populationInterval);

    if (!history.contains("points") || !history["points"].is_array()) {
        return;
    }
    for (const auto& entry : history["points"]) {
        if (!entry.is_array() || entry.size() < 3 || !entry[0].is_number_unsigned()) {
            continue;
        }
        WorldState::PopulationPoint point;
        point.tick = entry[0].get<std::uint64_t>();
        // Время в летописи может только идти вперёд: точка не новее
        // последней — это либо повтор (дельта разошлась с точкой отсчёта
        // сервера), либо мусор, и на оси времени она дала бы скачок назад.
        if (!working_.populationHistory.empty() && point.tick <= working_.populationHistory.back().tick) {
            continue;
        }
        point.plants = decodeIntArray(entry[1]);
        point.herbivores = decodeIntArray(entry[2]);
        // Четвёртый элемент появился вместе с хищниками: у точек из мира,
        // прожитого до них, его просто нет, и это не повод потерять точку.
        if (entry.size() > 3) {
            point.predators = decodeIntArray(entry[3]);
        }
        working_.populationHistory.push_back(std::move(point));
    }
}

// Карточка выбранного существа. Приходит только когда изменилась, поэтому
// отсутствие ключа означает "то же, что и было", а не "выбор снят": снятый
// выбор сервер присылает явным kind == "none".
void NetworkClient::applyWatched(const nlohmann::json& message) {
    if (!message.contains("watched") || !message["watched"].is_object()) {
        return;
    }
    const auto& watched = message["watched"];

    WorldState::Watched parsed;
    parsed.kind = watched.value("kind", std::string{});
    if (parsed.kind == "none") {
        working_.watched = WorldState::Watched{};
        return;
    }
    parsed.id = watched.value("id", static_cast<std::uint64_t>(0));
    parsed.x = watched.value("x", 0);
    parsed.y = watched.value("y", 0);
    parsed.species = watched.value("species", 0);
    parsed.diet = watched.value("diet", std::string{});
    parsed.sex = watched.value("sex", std::string{});
    parsed.desire = watched.value("desire", std::string{});

    if (watched.contains("groups") && watched["groups"].is_array()) {
        for (const auto& group : watched["groups"]) {
            if (!group.is_object() || !group.contains("values") || !group["values"].is_array()) {
                continue;
            }
            WorldState::WatchedGroup parsedGroup;
            parsedGroup.title = group.value("title", std::string{});
            for (const auto& entry : group["values"]) {
                // Пара "имя-число": порядок внутри группы осмысленный (тот
                // же, что в таблице черт ядра), поэтому массив, а не объект.
                if (!entry.is_array() || entry.size() < 2 || !entry[0].is_string() || !entry[1].is_number()) {
                    continue;
                }
                parsedGroup.values.emplace_back(entry[0].get<std::string>(), entry[1].get<float>());
            }
            parsed.groups.push_back(std::move(parsedGroup));
        }
    }

    // Округа и дорога хищника — плоскими парами координат: клеток в округе
    // сотни, и имена полей весили бы больше самих чисел (тот же приём, что
    // у точек летописи).
    auto readCells = [&watched](const char* field, std::vector<std::pair<int, int>>& out) {
        if (!watched.contains(field) || !watched[field].is_array()) {
            return;
        }
        const auto& array = watched[field];
        for (std::size_t i = 0; i + 1 < array.size(); i += 2) {
            if (!array[i].is_number() || !array[i + 1].is_number()) {
                continue;
            }
            out.emplace_back(array[i].get<int>(), array[i + 1].get<int>());
        }
    };
    readCells("reach", parsed.reach);
    readCells("road", parsed.road);
    parsed.roadKind = watched.value("road_kind", std::string{});
    parsed.roadX = watched.value("road_x", 0);
    parsed.roadY = watched.value("road_y", 0);

    working_.watched = std::move(parsed);
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
        // Мир пересоздаётся только через world_init, поэтому и признак
        // "мир сгенерирован" приходит только здесь — в дельте ему
        // взяться неоткуда.
        working_.generated = json.value("generated", true);

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

        // Доли и глубины приходят сотыми, счётные величины (минералы,
        // перегной, номера видов) — как есть. Деление на сто здесь и
        // только здесь: дальше по клиенту ходит доля 0..1.
        working_.moisture = decodeScaled(layers, "moisture", cellCount, kFromHundredths);
        working_.rockiness = decodeScaled(layers, "rockiness", cellCount, kFromHundredths);
        working_.height = decodeScaled(layers, "height", cellCount, kFromHundredths);
        working_.waterDepth = decodeScaled(layers, "water", cellCount, kFromHundredths);
        working_.minerals = decodeInts(layers, "minerals", cellCount, 0);
        working_.humus = decodeInts(layers, "humus", cellCount, 0);
        // -1 — пустая клетка (растение это Entity, и его отсутствие в
        // плотном массиве выражается значением-заглушкой).
        working_.plantSpeciesAt = decodeInts(layers, "species", cellCount, -1);
        working_.plantGrowth = decodeScaled(layers, "growth", cellCount, kFromHundredths);
        // Семена — тем же способом, что и растения: -1 значит "семени в
        // клетке нет".
        working_.seedSpeciesAt = decodeInts(layers, "seeds", cellCount, -1);
        working_.carcass = decodeScaled(layers, "carcass", cellCount, kFromHundredths);

        if (json.contains("plant_species")) {
            working_.plantSpecies.clear();
            for (const auto& archetype : json["plant_species"]) {
                if (!archetype.is_object()) {
                    continue;
                }
                std::vector<std::pair<std::string, int>> traits;
                for (const auto& [name, value] : archetype.items()) {
                    if (name != "species" && value.is_number()) {
                        traits.emplace_back(name, value.get<int>());
                    }
                }
                working_.plantSpecies.push_back(std::move(traits));
            }
        }

        // Виды животных и само поголовье — тем же способом, что и трава
        // выше: клиент не знает состава генома и не должен.
        auto readSpecies = [](const nlohmann::json& list) {
            std::vector<std::vector<std::pair<std::string, int>>> species;
            for (const auto& archetype : list) {
                if (!archetype.is_object()) {
                    continue;
                }
                std::vector<std::pair<std::string, int>> traits;
                for (const auto& [name, value] : archetype.items()) {
                    if (name != "species" && value.is_number()) {
                        traits.emplace_back(name, value.get<int>());
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
        // Летопись — заменой, а не дополнением: world_init означает, что
        // мир построен заново (регенерация, загрузка), и накопленное
        // относится к другому миру. Старый сервер её вовсе не пришлёт —
        // тогда график просто окажется пустым.
        applyPopulationHistory(json, /*replace=*/true);
        applyWatched(json);

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

        const auto toFraction = [](int raw) { return static_cast<float>(raw) * kFromHundredths; };
        applyChangedCells(json, "moisture", working_.moisture, toFraction);
        applyChangedCells(json, "height", working_.height, toFraction);
        applyChangedCells(json, "water", working_.waterDepth, toFraction);
        applyChangedCells(json, "growth", working_.plantGrowth, toFraction);
        applyChangedCells(json, "minerals", working_.minerals, [](int raw) { return raw; });
        applyChangedCells(json, "humus", working_.humus, [](int raw) { return raw; });
        applyChangedCells(json, "carcass", working_.carcass, toFraction);
        applyChangedCells(json, "species", working_.plantSpeciesAt, [](int raw) { return raw; });
        applyChangedCells(json, "seeds", working_.seedSpeciesAt, [](int raw) { return raw; });
        // Поголовье — изменениями, как и слои, только правится не клетка,
        // а животное (см. протокол в server/NetworkServer.hpp).
        applyAnimalChanges(json);
        applyPopulationHistory(json, /*replace=*/false);
        applyWatched(json);
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

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

// Размер взрослого приезжает тысячными (adult_size, core/Scale.hpp), а не
// сотыми: он единственное поле карточки, которое живёт в шкале мира, а не в
// шкале показа, и округлять его до сотых значило бы терять разницу между
// близкими видами.
constexpr float kFromThousandths = 0.001f;

// Слой в дельте — плоский массив пар "индекс тайла, новое значение"
// (см. протокол в server/NetworkServer.hpp). Индекс проверяется:
// сообщение приходит извне, и битые данные не должны приводить к записи
// за границы массива.
template <typename T, typename Decode>
void applyChangedCells(const nlohmann::json& message, const char* key, std::vector<T>& target,
                       std::vector<std::int32_t>& changed, Decode decode) {
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
            // Записали — а не "изменилось значение". Перезапись тем же числом
            // бывает редко (сервер шлёт клетку, только когда она изменилась на
            // единицу показа), а сличать старое с новым дороже, чем изредка
            // пересчитать цвет этой клетки зря.
            changed.push_back(static_cast<std::int32_t>(index));
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
    parsed.adultSize = animal.value("adult_size", 1000) * kFromThousandths;
    parsed.sex = animal.value("sex", std::string{});
    parsed.desire = animal.value("desire", std::string{});
    return parsed;
}

// Карточка гоблина. Своя, а не третий случай в звериной: у гоблина племя
// вместо диеты, и "kind" в его карточке нет вовсе — что в списке лежат
// гоблины, видно по самому списку.
WorldState::Goblin parseGoblin(const nlohmann::json& goblin) {
    WorldState::Goblin parsed;
    parsed.id = goblin.value("id", static_cast<std::uint64_t>(0));
    parsed.x = goblin.value("x", 0);
    parsed.y = goblin.value("y", 0);
    parsed.tribe = goblin.value("tribe", 0);
    parsed.growth = goblin.value("growth", 0) * kFromHundredths;
    parsed.health = goblin.value("health", 100) * kFromHundredths;
    parsed.fatigue = goblin.value("fatigue", 0) * kFromHundredths;
    parsed.carried = goblin.value("carried", 0) * kFromHundredths;
    parsed.material = goblin.value("material", 0) * kFromHundredths;
    parsed.sex = goblin.value("sex", std::string{});
    parsed.desire = goblin.value("desire", std::string{});
    return parsed;
}

// Шаг существа: куда оно повернулось и когда шагнуло.
//
// Один закон на зверя и на гоблина, и обязан быть одним: оба ходят по одной
// карте и оба рисуются повёрнутыми туда, куда шагнули. Скопировать его вторым
// разом значило бы завести два закона там, где он один.
//
// Читается ДО applyCreatureChanges: сторона — это разница между "было" и
// "стало", а после него "было" уже не осталось нигде. Индексы там и здесь
// одни и те же — места в ПРЕЖНЕМ списке, до удаления ушедших и вставки
// родившихся, — поэтому проход и стоит перед вызовом.
//
// Ни стороны, ни тика шага в протоколе нет вовсе: сам шаг виден по тому, что
// клетка приехала в дельте (см. WorldState::Animal).
//
// vertical зовётся на каждом шаге и получает 0, когда у шага была боковая
// составляющая. Отвесный кадр есть только у гоблина, и зверю о нём знать
// незачем — оттого это и вынесено наружу, а не решается здесь.
template <typename Card, typename Vertical>
void rememberSteps(std::vector<Card>& list, const nlohmann::json& changes, std::uint64_t tick,
                    Vertical&& vertical) {
    if (!changes.contains("pos") || !changes["pos"].is_array()) {
        return;
    }
    const auto& triples = changes["pos"];
    for (std::size_t p = 0; p + 2 < triples.size(); p += 3) {
        const auto index = triples[p].get<std::size_t>();
        if (index >= list.size()) {
            continue;
        }
        Card& card = list[index];
        const int x = triples[p + 1].get<int>();
        const int y = triples[p + 2].get<int>();
        // Боковая составляющая важнее отвесной, и потому спрашивается первой:
        // у косого шага есть обе, и со стороны такой шаг выглядит боковым, а
        // не отвесным.
        if (x != card.x) {
            card.facing = x > card.x ? 1 : -1;
            vertical(card, 0);
        } else if (y != card.y) {
            // Отвесный: боковой составляющей нет вовсе, и сторона остаётся
            // прежней — ей просто нечем смениться.
            vertical(card, y > card.y ? 1 : -1);
        }
        card.stepTick = tick;
    }
}

// Применение изменений к списку существ — ОДИН закон на всех, у кого такой
// список есть.
//
// Списки у зверя и у гоблина разные, а вот КАК из дельты получается новый
// список — совпадает до последней строки, и обязано совпадать: на той
// стороне провода их собирает один и тот же шаблон
// (creaturesDeltaJson в server/NetworkServer.cpp). Скопировать это вторым
// разом значит завести два закона там, где он один, — а разъехавшись, они
// поставят существо на карте не туда, где оно есть в мире, и заметить это
// будет нечем.
//
// Порядок обязателен и он же порядок ключей в сообщении: сперва правки по
// индексам, потом удаление ушедших (их индексы — в том же прежнем списке),
// и только потом вставка родившихся.
// extra получает сам applyPairs и правит им поля, которых у зверя нет
// (усталость гоблина). Зовётся между правками и удалениями — там же, где
// правятся остальные поля: индексы во всех них считаны в ПРЕЖНЕМ списке, и
// после удаления ушедших они уже ничего не значат.
template <typename Card, typename Parse, typename Extra>
void applyCreatureChanges(std::vector<Card>& list, const nlohmann::json& changes, Parse&& parse, Extra&& extra) {
    // Пары "индекс - значение", как у тайловых слоёв, только правится не
    // клетка, а существо. Индекс проверяется: сообщение приходит извне.
    const auto applyPairs = [&](const char* key, auto&& write) {
        if (!changes.contains(key) || !changes[key].is_array()) {
            return;
        }
        const auto& pairs = changes[key];
        for (std::size_t p = 0; p + 1 < pairs.size(); p += 2) {
            const auto index = pairs[p].get<std::size_t>();
            if (index < list.size()) {
                write(list[index], pairs[p + 1]);
            }
        }
    };

    if (changes.contains("pos") && changes["pos"].is_array()) {
        const auto& triples = changes["pos"];
        for (std::size_t p = 0; p + 2 < triples.size(); p += 3) {
            const auto index = triples[p].get<std::size_t>();
            if (index < list.size()) {
                list[index].x = triples[p + 1].get<int>();
                list[index].y = triples[p + 2].get<int>();
            }
        }
    }
    applyPairs("growth", [](Card& c, const nlohmann::json& v) { c.growth = v.get<int>() * kFromHundredths; });
    applyPairs("health", [](Card& c, const nlohmann::json& v) { c.health = v.get<int>() * kFromHundredths; });
    applyPairs("desire", [](Card& c, const nlohmann::json& v) {
        if (v.is_string()) {
            c.desire = v.get<std::string>();
        }
    });
    extra(applyPairs);

    if (changes.contains("gone") && changes["gone"].is_array()) {
        // Пометить и выбросить одним проходом: удалять по одному значило бы
        // сдвигать хвост списка на каждом ушедшем, а индексы в сообщении
        // считаны в списке до всяких удалений.
        std::vector<bool> gone(list.size(), false);
        for (const auto& index : changes["gone"]) {
            const auto i = index.get<std::size_t>();
            if (i < gone.size()) {
                gone[i] = true;
            }
        }
        std::vector<Card> kept;
        kept.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!gone[i]) {
                kept.push_back(std::move(list[i]));
            }
        }
        list = std::move(kept);
    }

    if (changes.contains("born") && changes["born"].is_array()) {
        // Родившиеся приходят полными карточками и в порядке id, остаток
        // списка в том же порядке — значит слияние, а не сортировка.
        std::vector<Card> born;
        for (const auto& record : changes["born"]) {
            if (record.is_object()) {
                born.push_back(parse(record));
            }
        }
        std::vector<Card> merged;
        merged.reserve(list.size() + born.size());
        std::merge(std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()),
                   std::make_move_iterator(born.begin()), std::make_move_iterator(born.end()),
                   std::back_inserter(merged), [](const Card& a, const Card& b) { return a.id < b.id; });
        list = std::move(merged);
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
    // Версию карты тут не трогаем: снимок публикуется на всякое сообщение
    // сервера, а карта меняется только от клеток (см. WorldState::mapVersion).
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
    // Отвесный шаг зверю не нужен: кадров со спины у него нет (WolfSprites).
    rememberSteps(working_.animals, changes, working_.tick, [](WorldState::Animal&, int) {});
    applyCreatureChanges(working_.animals, changes, parseAnimal, [](auto&&) {});
}

void NetworkClient::applyGoblins(const nlohmann::json& message) {
    if (!message.contains("goblins") || !message["goblins"].is_array()) {
        return;
    }
    working_.goblins.clear();
    for (const auto& goblin : message["goblins"]) {
        if (!goblin.is_object()) {
            continue;
        }
        working_.goblins.push_back(parseGoblin(goblin));
    }
}

void NetworkClient::applyGoblinChanges(const nlohmann::json& message) {
    if (!message.contains("goblins") || !message["goblins"].is_object()) {
        return;
    }
    const auto& changes = message["goblins"];
    rememberSteps(working_.goblins, changes, working_.tick,
                   [](WorldState::Goblin& goblin, int vertical) { goblin.verticalStep = vertical; });
    applyCreatureChanges(working_.goblins, changes, parseGoblin, [](auto&& applyPairs) {
        applyPairs("fatigue", [](WorldState::Goblin& g, const nlohmann::json& v) {
            g.fatigue = v.get<int>() * kFromHundredths;
        });
        applyPairs("carried", [](WorldState::Goblin& g, const nlohmann::json& v) {
            g.carried = v.get<int>() * kFromHundredths;
        });
        applyPairs("material", [](WorldState::Goblin& g, const nlohmann::json& v) {
            g.material = v.get<int>() * kFromHundredths;
        });
    });
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
    applyPopulationTraits(history);

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
        // Средние геномы дописаны ещё позже, тем же способом — с пятого по
        // седьмой. У точек постарше их нет, и график генома такую часть
        // кривой просто не начинает.
        if (entry.size() > 6) {
            point.plantGenome = decodeIntArray(entry[4]);
            point.herbivoreGenome = decodeIntArray(entry[5]);
            point.predatorGenome = decodeIntArray(entry[6]);
        }
        // Деревья дописаны ещё позже — восьмым и девятым. У точек из мира,
        // прожитого без них, их нет, и это не повод потерять точку: график
        // рощ такую часть кривой просто не начинает.
        if (entry.size() > 8) {
            point.trees = decodeIntArray(entry[7]);
            point.treeGenome = decodeIntArray(entry[8]);
        }
        // Гоблины — десятым и одиннадцатым, тем же способом. Элемент в
        // конец: имена полей у тысячи точек весили бы больше самих чисел.
        if (entry.size() > 10) {
            point.goblins = decodeIntArray(entry[9]);
            point.goblinGenome = decodeIntArray(entry[10]);
        }
        // Кусты — двенадцатым и тринадцатым, тем же способом и с той же
        // оговоркой: у точек из мира без ягодников их нет.
        if (entry.size() > 12) {
            point.bushes = decodeIntArray(entry[11]);
            point.bushGenome = decodeIntArray(entry[12]);
        }
        working_.populationHistory.push_back(std::move(point));
    }
}

// Черты, по которым записан средний геном: имя и границы вложения.
// Приходят только в world_init (таблица черт вкомпилирована в ядро и за
// жизнь мира не меняется), поэтому и читаются здесь же, где летопись.
void NetworkClient::applyPopulationTraits(const nlohmann::json& history) {
    if (!history.contains("traits") || !history["traits"].is_object()) {
        return;
    }
    const auto read = [](const nlohmann::json& json) {
        std::vector<WorldState::PopulationTrait> traits;
        if (!json.is_array()) {
            return traits;
        }
        for (const auto& entry : json) {
            if (!entry.is_object()) {
                continue;
            }
            WorldState::PopulationTrait trait;
            trait.name = entry.value("name", std::string{});
            trait.lo = entry.value("lo", 0);
            trait.hi = entry.value("hi", 0);
            traits.push_back(std::move(trait));
        }
        return traits;
    };
    const auto& traits = history["traits"];
    working_.plantTraits = read(traits.contains("plants") ? traits["plants"] : nlohmann::json{});
    working_.treeTraits = read(traits.contains("trees") ? traits["trees"] : nlohmann::json{});
    working_.bushTraits = read(traits.contains("bushes") ? traits["bushes"] : nlohmann::json{});
    working_.herbivoreTraits = read(traits.contains("herbivores") ? traits["herbivores"] : nlohmann::json{});
    working_.predatorTraits = read(traits.contains("predators") ? traits["predators"] : nlohmann::json{});
    working_.goblinTraits = read(traits.contains("goblins") ? traits["goblins"] : nlohmann::json{});
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
    parsed.doing = watched.value("doing", std::string{});

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
                parsedGroup.values.emplace_back(entry[0].get<std::string>(), entry[1].get<int>());
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
    // Пригодность отдыха — тройками "клетка и число", а не парами: у неё
    // есть значение, а не только место.
    parsed.knows.clear();
    if (watched.contains("knows") && watched["knows"].is_array()) {
        for (const auto& place : watched["knows"]) {
            if (!place.is_object()) {
                continue;
            }
            parsed.knows.push_back(WorldState::Watched::Known{place.value("x", 0), place.value("y", 0),
                                                               place.value("kind", std::string{}),
                                                               place.value("strength", 0)});
        }
    }

    parsed.rest.clear();
    if (watched.contains("rest") && watched["rest"].is_array()) {
        const auto& triples = watched["rest"];
        for (std::size_t i = 0; i + 2 < triples.size(); i += 3) {
            parsed.rest.emplace_back(triples[i].get<int>(), triples[i + 1].get<int>(),
                                      triples[i + 2].get<int>());
        }
    }
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
        working_.trampled = decodeScaled(layers, "trampled", cellCount, kFromHundredths);
        working_.height = decodeScaled(layers, "height", cellCount, kFromHundredths);
        // Диапазон подсветки рельефа считается один раз здесь, а не на
        // каждую пересборку текстуры (MapTexture) — иначе эрозия,
        // подъедающая вершины каждый тик, сужала бы диапазон и заставляла
        // бы весь остальной рельеф мерцать светлее вслед за одной
        // проседающей горой (см. heightMin/heightMax в NetworkClient.hpp).
        if (working_.height.empty()) {
            working_.heightMin = working_.heightMax = 0.0f;
        } else {
            const auto [minIt, maxIt] = std::minmax_element(working_.height.begin(), working_.height.end());
            working_.heightMin = *minIt;
            working_.heightMax = *maxIt;
        }
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
        // Деревья — тем же способом: -1 значит "дерева в клетке нет".
        working_.treeSpeciesAt = decodeInts(layers, "trees", cellCount, -1);
        working_.bushSpeciesAt = decodeInts(layers, "bushes", cellCount, -1);
        // Ягоды — счётные штуки, как минералы и перегной: ни округления
        // показа, ни перевода долей (shared/protocol/WirePrecision.hpp).
        working_.berries = decodeInts(layers, "berries", cellCount, 0);
        working_.store = decodeInts(layers, "store", cellCount, 0);
        working_.canopy = decodeScaled(layers, "canopy", cellCount, kFromHundredths);
        working_.bedding = decodeScaled(layers, "bedding", cellCount, kFromHundredths);
        working_.site = decodeInts(layers, "site", cellCount, 0);
        working_.siteMaterial = decodeInts(layers, "site_material", cellCount, 0);
        working_.carcass = decodeScaled(layers, "carcass", cellCount, kFromHundredths);

        // Виды растений: два списка, у травы и у деревьев своя нумерация.
        // Состав генома клиент не знает и не должен (07_TechStack.md, п.6) —
        // читается всё, что прислали, парами "имя -> значение".
        auto readPlantSpecies = [](const nlohmann::json& list) {
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
        if (json.contains("plant_species")) {
            working_.plantSpecies = readPlantSpecies(json["plant_species"]);
        }
        if (json.contains("tree_species")) {
            working_.treeSpecies = readPlantSpecies(json["tree_species"]);
        }
        if (json.contains("bush_species")) {
            working_.bushSpecies = readPlantSpecies(json["bush_species"]);
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
        applyGoblins(json);
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
        // Мир приехал целиком — все слои переписаны, и карта заведомо другая.
        // Списка изменившихся клеток у него нет: менялось всё сразу, и пустой
        // список при новой версии для MapTexture и значит "пересчитай всё".
        working_.changedCells.clear();
        ++working_.mapVersion;
        publishState();
    } else if (type == "world_delta") {
        // Только изменившиеся клетки — накладываются на то, что уже
        // накоплено. Каменистость, булыжники, источники и виды травы сюда
        // не входят: они меняются лишь регенерацией, а она присылает
        // новый world_init.
        working_.tick = json.value("tick", working_.tick);
        working_.paused = json.value("paused", working_.paused);

        const auto toFraction = [](int raw) { return static_cast<float>(raw) * kFromHundredths; };
        // Клеточные слои — через одну руку, и она же копит список
        // изменившихся клеток. Не ради краткости: слой, добавленный сюда
        // обычным вызовом applyChangedCells, молча не попал бы в список, и
        // карта отставала бы на этот слой до следующего world_init — а
        // заметить такое почти нечем. Через общую руку такой промах
        // невозможен.
        working_.changedCells.clear();
        const auto cells = [&](const char* key, auto& target, auto decode) {
            applyChangedCells(json, key, target, working_.changedCells, decode);
        };
        const auto asIs = [](int raw) { return raw; };
        cells("moisture", working_.moisture, toFraction);
        cells("trampled", working_.trampled, toFraction);
        cells("height", working_.height, toFraction);
        cells("water", working_.waterDepth, toFraction);
        cells("growth", working_.plantGrowth, toFraction);
        cells("minerals", working_.minerals, asIs);
        cells("humus", working_.humus, asIs);
        cells("carcass", working_.carcass, toFraction);
        cells("species", working_.plantSpeciesAt, asIs);
        cells("seeds", working_.seedSpeciesAt, asIs);
        cells("trees", working_.treeSpeciesAt, asIs);
        cells("bushes", working_.bushSpeciesAt, asIs);
        cells("berries", working_.berries, asIs);
        cells("store", working_.store, asIs);
        cells("canopy", working_.canopy, toFraction);
        cells("bedding", working_.bedding, toFraction);
        cells("site", working_.site, asIs);
        cells("site_material", working_.siteMaterial, asIs);
        // Поголовье — изменениями, как и слои, только правится не клетка,
        // а животное (см. протокол в server/NetworkServer.hpp).
        applyAnimalChanges(json);
        applyGoblinChanges(json);
        applyPopulationHistory(json, /*replace=*/false);
        applyWatched(json);
        // Дельта, в которой шевельнулись только звери, карту не трогает: они
        // рисуются фигурами ПОВЕРХ неё, а не текселем (см. MapTexture).
        if (!working_.changedCells.empty()) {
            ++working_.mapVersion;
        }
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

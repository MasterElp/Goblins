#include "server/NetworkServer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "core/Hunting.hpp"
#include "core/Mating.hpp"
#include "core/Needs.hpp"
#include "core/Random.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HeightComponent.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/InjuryComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SeedComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"
#include "protocol/WirePrecision.hpp"
#include "server/WorldSave.hpp"

namespace goblins {

namespace {

// Порог невыбранной очереди отправки у клиента, при котором очередная
// рассылка пропускается: клиент не успевает принимать, и копить для него
// данные бессмысленно. Изменения не теряются — следующая дельта считается
// от последнего реально отправленного состояния и включит в себя
// пропущенное. Не настройка: это свойство транспорта, а не мира, и от
// мира к миру оно не меняется.
constexpr std::size_t kSnapshotBacklogBytes = 1024 * 1024;

// Перевода в тысячные доли (прежний encodeMilli) здесь больше нет и быть
// не может: мир и так целый (core/Scale.hpp), и поле "scale" из протокола
// исчезло вместе с ним. Округление до сотых (toWire) — не шкала, а
// точность показа, и живёт отдельно: см. shared/protocol/WirePrecision.hpp.

// Дельта одного слоя — плоский массив пар "индекс тайла, новое
// значение". Пустой, если слой не изменился вовсе; вызывающая сторона
// такой слой в сообщение не кладёт.
nlohmann::json changedCells(const std::vector<int>& previous, const std::vector<int>& current) {
    auto pairs = nlohmann::json::array();
    const std::size_t count = std::min(previous.size(), current.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (previous[i] != current[i]) {
            pairs.push_back(i);
            pairs.push_back(current[i]);
        }
    }
    return pairs;
}

} // namespace

void NetworkServer::LayerSnapshot::resize(int w, int h) {
    width = w;
    height = h;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    moisture.assign(count, 0);
    minerals.assign(count, 0);
    terrainHeight.assign(count, 0);
    water.assign(count, 0);
    humus.assign(count, 0);
    carcass.assign(count, 0);
    growth.assign(count, 0);
    rockiness.assign(count, 0);
    // -1 — клетка пуста: растение это Entity, и его отсутствие в плотном
    // массиве выражается значением-заглушкой. Семена — отдельным слоем и
    // тем же способом: семя лежит в той же клетке, где стоит растение
    // (обычно его родитель), и одним слоем эти два состояния клетки
    // выразить нельзя.
    species.assign(count, -1);
    seeds.assign(count, -1);
    trees.assign(count, -1);
    // Животные — список, а не слой (см. NetworkServer.hpp): он собирается
    // заново на каждый снимок, поэтому здесь только очищается.
    animals.clear();
}

NetworkServer::NetworkServer(const World& world, const PopulationHistory& history, const std::string& host, int port,
                              std::atomic<bool>& paused, ServerConfig baseConfig, std::string configPath,
                              std::filesystem::path savesDirectory)
    : world_(world), history_(history), server_(port, host), paused_(paused), baseConfig_(std::move(baseConfig)),
      configPath_(std::move(configPath)), savesDirectory_(std::move(savesDirectory)) {
    server_.setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*state*/,
               ix::WebSocket& webSocket,
               const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                // Список миров — только чтение каталога сохранений, его
                // можно отдать прямо здесь. А вот состояние мира —
                // нельзя: этот колбэк выполняется на потоке IXWebSocket,
                // и чтение ECS registry параллельно с идущим тиком было
                // бы гонкой. Поэтому только помечаем, что нужен полный
                // ресинк, — его соберёт и разошлёт поток GameLoop
                // (publish), как и всё остальное состояние мира.
                webSocket.send(buildWorldListMessage());
                requestFullResync();
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                // Отвернувшийся клиент, который ушёл совсем, не должен
                // оставлять за собой запись: указатели соединений
                // переиспользуются, и новый клиент занял бы место старого
                // уже отвернувшимся.
                std::lock_guard<std::mutex> lock(suspendedMutex_);
                suspended_.erase(&webSocket);
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                handleClientMessage(msg->str, &webSocket);
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

void NetworkServer::requestFullResync() {
    needsFullResync_.store(true);
}

void NetworkServer::publish(bool force) {
    if (!anyoneWatching()) {
        // Мир существует независимо от наблюдателя (02_CorePrinciples.md,
        // п.1) и продолжает тикать — но сериализовать его сейчас некому,
        // а это самая дорогая часть тика. Никто не подключён или все
        // отвернулись (свёрнутое окно, окно без фокуса) — разницы нет.
        // Точка отсчёта для дельт при этом протухает, поэтому вернувшийся
        // клиент получит полный world_init (его же запрашивает и колбэк
        // Open).
        sentValid_ = false;
        return;
    }

    bool full = needsFullResync_.load() || !sentValid_;

    const auto now = std::chrono::steady_clock::now();
    if (!full && !force) {
        const std::chrono::milliseconds interval(std::max(0, baseConfig_.snapshot_interval_ms));
        if (now - lastPublish_ < interval) {
            return;
        }
    }

    // Клиент не разгрёб предыдущее — новое ему сейчас не нужно. Точка
    // отсчёта не сдвигается, так что пропущенные изменения войдут в
    // следующую дельту; полный ресинк, если он был запрошен, останется
    // запрошенным и уйдёт, когда очередь разгребётся.
    if (clientsAreBehind()) {
        return;
    }

    captureLayers(current_);
    if (sent_.width != current_.width || sent_.height != current_.height) {
        // Размер Области сменился (загружен мир другого размера) —
        // дельта на такое состояние лечь не может.
        full = true;
    }

    // Подробности выбранного существа собираются один раз на рассылку:
    // они нужны и world_init, и дельте, а чтение registry не бесплатно.
    const auto watched = buildWatchedJson();
    const std::string watchedDump = watched.is_null() ? std::string{} : watched.dump();

    std::string payload;
    if (full) {
        payload = buildInitMessage(current_, watched);
    } else {
        payload = buildDeltaMessage(sent_, current_, watched, watchedDump != sentWatched_);
        if (payload.empty()) {
            lastPublish_ = now;
            return;
        }
    }

    broadcastSnapshot(payload);

    std::swap(sent_, current_);
    sentValid_ = true;
    sentTick_ = world_.registry().get<const TimeComponent>(world_.worldEntity()).tick;
    sentPaused_ = paused_.load();
    sentWatched_ = watchedDump;
    // Точка отсчёта для летописи двигается только теперь, вместе с
    // остальным состоянием: пропущенная рассылка (клиент не разгрёб
    // очередь) не должна проглотить точки — они уйдут следующей дельтой.
    // Пустая летопись отправленной не считается: первая точка мира стоит
    // на нулевом тике, и "новее нуля" её бы не пропустило.
    if (history_.points().empty()) {
        sentHistoryValid_ = false;
    } else {
        sentHistoryTick_ = history_.points().back().tick;
        sentHistoryInterval_ = history_.interval();
        sentHistoryValid_ = true;
    }
    lastPublish_ = now;
    if (full) {
        // Снимаем флаг только теперь, когда world_init действительно
        // отправлен: иначе следующая дельта легла бы клиенту на пустое
        // место.
        needsFullResync_.store(false);
    }
}

bool NetworkServer::clientsAreBehind() {
    for (const auto& client : server_.getClients()) {
        if (client->bufferedAmount() > kSnapshotBacklogBytes) {
            return true;
        }
    }
    return false;
}

void NetworkServer::captureLayers(LayerSnapshot& out) const {
    const int width = world_.area().width();
    out.resize(width, world_.area().height());

    const auto& registry = world_.registry();

    registry.view<const PositionComponent, const SoilComponent>().each(
        [&](const PositionComponent& pos, const SoilComponent& soil) {
            const std::size_t i = static_cast<std::size_t>(pos.y) * width + pos.x;
            // Доли — с точностью показа (toWire), минералы — как есть:
            // они счётные, а не доля (shared/protocol/WirePrecision.hpp).
            out.moisture[i] = toWire(soil.moisture);
            out.rockiness[i] = toWire(soil.rockiness);
            out.minerals[i] = soil.minerals;
        });

    // Высота рельефа (HeightComponent всегда идёт в паре с
    // SoilComponent на террейн-Entity, см. WorldSave.hpp) — нужна
    // клиенту для необязательного слоя-рельефа; единиц измерения не
    // несёт, клиент нормализует по min/max текущей карты.
    registry.view<const PositionComponent, const HeightComponent>().each(
        [&](const PositionComponent& pos, const HeightComponent& h) {
            out.terrainHeight[static_cast<std::size_t>(pos.y) * width + pos.x] = toWire(h.height);
        });

    // Вода и перегной на сервере разреженны (нет компонента = нет
    // возможности, 02_CorePrinciples.md, п.3), но по сети идут плотными
    // массивами, как и почва: ноль значит "нет". Так дельта одинаково
    // выражает и появление воды, и её уход.
    registry.view<const PositionComponent, const WaterComponent>().each(
        [&](const PositionComponent& pos, const WaterComponent& w) {
            out.water[static_cast<std::size_t>(pos.y) * width + pos.x] = toWire(w.depth);
        });

    registry.view<const PositionComponent, const HumusComponent>().each(
        [&](const PositionComponent& pos, const HumusComponent& tileHumus) {
            out.humus[static_cast<std::size_t>(pos.y) * width + pos.x] = tileHumus.minerals;
        });

    // Падаль — такое же состояние тайла, как перегной, и уходит таким же
    // плотным слоем: ноль значит "туши здесь нет".
    registry.view<const PositionComponent, const CarcassComponent>().each(
        [&](const PositionComponent& pos, const CarcassComponent& tileCarcass) {
            out.carcass[static_cast<std::size_t>(pos.y) * width + pos.x] = toWire(tileCarcass.meat);
        });

    registry.view<const PositionComponent, const PlantComponent, const PlantGenomeComponent>().each(
        [&](const entt::entity entity, const PositionComponent& pos, const PlantComponent& plant,
            const PlantGenomeComponent& genome) {
            const std::size_t i = static_cast<std::size_t>(pos.y) * width + pos.x;
            out.growth[i] = toWire(plant.growth);
            if (registry.all_of<TreeComponent>(entity)) {
                out.trees[i] = genome.species;
            } else {
                out.species[i] = genome.species;
            }
        });

    // Семена (SeedComponent) — вид того, что из семени вырастет, или -1.
    // Возраст и срок покоя по сети не идут: клиенту нужно знать, что в
    // клетке лежит семя, а не сколько ему осталось спать.
    registry.view<const PositionComponent, const SeedComponent, const PlantGenomeComponent>().each(
        [&](const PositionComponent& pos, const SeedComponent& /*seed*/, const PlantGenomeComponent& genome) {
            out.seeds[static_cast<std::size_t>(pos.y) * width + pos.x] = genome.species;
        });

    // Животные — разреженным списком, а не слоем: на одной клетке их может
    // стоять несколько, и плотный массив по определению не смог бы этого
    // выразить. Сортируем по клетке и виду, чтобы порядок в списке не
    // зависел от того, в каком порядке EnTT хранит Entity: иначе дельта
    // видела бы изменение там, где мир не менялся вовсе.
    registry
        .view<const PositionComponent, const AnimalComponent, const AnimalGenomeComponent, const DesireComponent>()
        .each([&](const entt::entity entity, const PositionComponent& pos, const AnimalComponent& animal,
                   const AnimalGenomeComponent& genome, const DesireComponent& desire) {
            const auto* identity = registry.try_get<const IdentityComponent>(entity);
            // По имени поля, а не по порядку: полей девять, половина из
            // них int, и перепутанные местами вид с развитостью собрались
            // бы молча.
            out.animals.push_back(LayerSnapshot::AnimalView{
                .id = identity != nullptr ? identity->id : 0,
                .x = pos.x,
                .y = pos.y,
                .growth = toWire(animal.growth),
                .health = toWire(animal.health),
                .desire = desire.current,
                .species = genome.species,
                .predator = registry.all_of<PredatorComponent>(entity),
                .sex = animal.sex});
        });
    // По идентификатору, и только по нему. Порядок обязан быть одинаковым
    // от тика к тику — иначе дельта видела бы изменение там, где мир не
    // менялся, — а из всего, что есть у животного, постоянен один лишь id:
    // клетка меняется каждый шаг, желание и здоровье тоже. Он же и ключ
    // дельты: список, отсортированный по id, правится по индексам без
    // всякого поиска (см. "animals" в описании дельты).
    std::sort(out.animals.begin(), out.animals.end(),
              [](const LayerSnapshot::AnimalView& a, const LayerSnapshot::AnimalView& b) {
                  return a.id < b.id;
              });
}

// Карточка одного животного — всё, что о нём знает клиент. Одна на
// world_init и на "born" дельты: разойтись эти два места не должны.
nlohmann::json NetworkServer::animalToJson(const LayerSnapshot::AnimalView& animal) {
    return {{"id", animal.id},
            {"x", animal.x},
            {"y", animal.y},
            {"species", animal.species},
            {"kind", animal.predator ? "predator" : "herbivore"},
            {"growth", animal.growth},
            {"health", animal.health},
            {"sex", sexName(animal.sex)},
            {"desire", desireName(animal.desire)}};
}

nlohmann::json NetworkServer::animalsToJson(const std::vector<LayerSnapshot::AnimalView>& animals) {
    auto array = nlohmann::json::array();
    for (const auto& animal : animals) {
        array.push_back(animalToJson(animal));
    }
    return array;
}

nlohmann::json NetworkServer::animalsDeltaJson(const std::vector<LayerSnapshot::AnimalView>& previous,
                                                const std::vector<LayerSnapshot::AnimalView>& current) {
    auto gone = nlohmann::json::array();
    auto born = nlohmann::json::array();
    auto pos = nlohmann::json::array();
    auto growth = nlohmann::json::array();
    auto health = nlohmann::json::array();
    auto desire = nlohmann::json::array();

    // Оба списка отсортированы по id (см. captureLayers), поэтому сравнение
    // — обычное слияние: ушедший есть только слева, родившийся — только
    // справа, а тот, кто есть с обеих сторон, сравнивается по полям.
    std::size_t p = 0;
    std::size_t c = 0;
    while (p < previous.size() || c < current.size()) {
        if (c == current.size() || (p < previous.size() && previous[p].id < current[c].id)) {
            gone.push_back(p);
            ++p;
            continue;
        }
        if (p == previous.size() || current[c].id < previous[p].id) {
            born.push_back(animalToJson(current[c]));
            ++c;
            continue;
        }
        const auto& was = previous[p];
        const auto& now = current[c];
        // Индекс — место в ПРЕЖНЕМ списке: клиент правит тот список,
        // который у него уже есть, и делает это до удалений и вставок.
        if (was.x != now.x || was.y != now.y) {
            pos.push_back(p);
            pos.push_back(now.x);
            pos.push_back(now.y);
        }
        if (was.growth != now.growth) {
            growth.push_back(p);
            growth.push_back(now.growth);
        }
        if (was.health != now.health) {
            health.push_back(p);
            health.push_back(now.health);
        }
        if (was.desire != now.desire) {
            desire.push_back(p);
            desire.push_back(desireName(now.desire));
        }
        // Вид, диета и пол животного не меняются за всю его жизнь, поэтому
        // в дельте их нет вовсе: они приезжают один раз, в карточке.
        ++p;
        ++c;
    }

    nlohmann::json message = nlohmann::json::object();
    // Порядок ключей здесь и есть порядок применения на клиенте
    // (правки -> удаления -> вставки), и он же описан в протоколе.
    if (!pos.empty()) message["pos"] = std::move(pos);
    if (!growth.empty()) message["growth"] = std::move(growth);
    if (!health.empty()) message["health"] = std::move(health);
    if (!desire.empty()) message["desire"] = std::move(desire);
    if (!gone.empty()) message["gone"] = std::move(gone);
    if (!born.empty()) message["born"] = std::move(born);
    return message.empty() ? nlohmann::json{} : message;
}

namespace {

// Группа значений в "watched": заголовок и пары "имя-число" в осмысленном
// порядке (тем же, в котором они перечислены в таблице черт или в самом
// компоненте). Массив пар, а не объект: у объекта порядок ключей теряется,
// а читать геном вразнобой — совсем не то же самое, что по таблице.
// Значения — целые: мир целочислен (core/Scale.hpp), и панель наблюдения
// показывает ровно то, чем он живёт, без перевода в доли по дороге.
nlohmann::json makeGroup(const char* title, std::initializer_list<std::pair<const char*, int>> values) {
    auto pairs = nlohmann::json::array();
    for (const auto& [name, value] : values) {
        auto pair = nlohmann::json::array();
        pair.push_back(name);
        pair.push_back(value);
        pairs.push_back(std::move(pair));
    }
    return nlohmann::json{{"title", title}, {"values", std::move(pairs)}};
}

// Что лежит на тайле из того, что нужно закону охоты: есть ли под ногами
// земля, стоит ли на ней вода, лежит ли падаль. Наблюдателю нужно то же
// самое, что и системе тика, но по клетке за раз: собирать снимок всей
// Области ради одного выбранного зверя незачем — за одну рассылку сюда
// приходит меньше тысячи клеток, а не десятки тысяч.
struct TileFacts {
    bool soil = false;
    int water = 0;
    int carcass = 0;
};

TileFacts tileFactsAt(const World& world, int x, int y) {
    TileFacts facts;
    if (!world.area().inBounds(x, y)) {
        return facts;
    }
    const auto& registry = world.registry();
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (!registry.all_of<SoilComponent>(entity)) {
            continue;
        }
        facts.soil = true;
        if (const auto* water = registry.try_get<const WaterComponent>(entity)) {
            facts.water = water->depth;
        }
        if (const auto* carcass = registry.try_get<const CarcassComponent>(entity)) {
            facts.carcass = carcass->meat;
        }
        break;
    }
    return facts;
}

// Дорога выбранного зверя — та же самая, по которой он идёт сам.
//
// Считается она не копией правил, а теми же законами (core/Path.hpp,
// core/Hunting.hpp, core/Mating.hpp) и из тех же чисел: клетка, зоркость,
// скорость, голод из тела (core/Needs.hpp) и розыгрыш, собранный ровно так
// же, как его собирает AnimalSystem (seed мира, тик, идентификатор зверя).
// Иначе нарисованная дорога рано или поздно разошлась бы с настоящей, а
// дорога, по которой зверь не идёт, хуже ненарисованной: по ней в первую
// очередь и судят, работает ли поиск пути вообще.
//
// "reach" — вся округа, до которой у зверя есть ход: по ней сразу видно,
// почему он не пошёл за тем, что стоит на виду за рекой. Она есть у всякого
// животного; дорога — только у того, кто сейчас куда-то идёт: хищник за
// добычей или всякий зверь за парой. За травой и за водой дороги не
// считают — туда идут напролом и обходят преграду вслепую (09_Animals.md,
// п.8), и рисовать там дорогу значило бы показывать решение, которого зверь
// не принимал.
void appendRoad(const World& world, entt::entity entity, const AnimalComponent& animal,
                const AnimalGenomeComponent& genome, const PositionComponent& position, Desire desire,
                bool predator, std::uint64_t id, nlohmann::json& watched, nlohmann::json& groups) {
    const auto& registry = world.registry();
    const auto& properties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const int sight = std::max(1, genome.perception);

    Reach reach;
    reach.build(world.area(), position.x, position.y, sight, [&world](int x, int y) {
        const TileFacts facts = tileFactsAt(world, x, y);
        return standableAt(world.area().isBlocked(x, y), facts.soil, facts.water);
    });

    std::vector<PathCell> cells;
    reach.reachedCells(cells);
    const int reachCount = static_cast<int>(cells.size());
    auto reachJson = nlohmann::json::array();
    for (const auto& cell : cells) {
        reachJson.push_back(cell.x);
        reachJson.push_back(cell.y);
    }
    watched["reach"] = std::move(reachJson);

    // Те же две величины числами, рядом с телом и геномом: "докуда есть
    // ход" и "сколько шагов до цели". Ноль шагов при полной округе — это и
    // есть "не нашлось никого, до кого можно дойти", и увидеть это в
    // карточке проще, чем вглядываться в карту.
    // "stuck" — то, что животное помнит ногами (core/Walk.hpp): насколько
    // давно у него нет продвижения. Число рядом с дорогой, потому что
    // отвечает оно на тот же вопрос: почему зверь стоит или ходит кругами.
    const auto* memory = registry.try_get<const MovementComponent>(entity);
    auto pathGroup = [&groups, reachCount, memory](int roadSteps) {
        groups.push_back(makeGroup("Path", {{"reach_cells", reachCount},
                                             {"road_steps", roadSteps},
                                             {"stuck", memory != nullptr ? memory->stuck : 0}}));
    };

    // Куда идёт зверь: цель и её род. Считают их два разных закона, но
    // дальше с ними делают одно и то же, поэтому и сходятся они здесь.
    bool hasTarget = false;
    int targetX = 0;
    int targetY = 0;
    const char* kind = "";
    // Отдельно от kind: зов рисуется прямой линией, а не дорогой (см. ниже,
    // у reach.roadTo) — bool дешевле и надёжнее сравнения строк.
    bool isCall = false;

    if (predator && desire == Desire::Food) {
        std::vector<HuntPrey> preys;
        // Гоблины из списка добычи исключены, и это не украшение: их не
        // видит сама AnimalSystem (см. её список preys), а нарисованная
        // наблюдателю дорога обязана вести туда же, куда пойдёт хищник.
        for (const auto other : registry.view<const AnimalComponent, const AnimalGenomeComponent,
                                              const PositionComponent>(entt::exclude<GoblinComponent>)) {
            if (other == entity || registry.all_of<PredatorComponent>(other)) {
                continue;
            }
            const auto& preyPosition = registry.get<const PositionComponent>(other);
            const auto& preyGenome = registry.get<const AnimalGenomeComponent>(other);
            // Под кроной добычу не высматривают (kCoverSight,
            // core/Hunting.hpp). Наблюдатель обязан видеть ровно то же, что
            // и хищник, иначе нарисованная дорога разойдётся с настоящей.
            bool underTree = false;
            for (const auto tile : world.area().cellAt(preyPosition.x, preyPosition.y).entities) {
                if (registry.all_of<PlantComponent, TreeComponent>(tile)) {
                    underTree = true;
                    break;
                }
            }
            preys.push_back(
                HuntPrey{preyPosition.x, preyPosition.y, preyGenome.speed, preyGenome.defense, underTree});
        }

        const HuntChoice choice = chooseHuntTarget(
            reach, Hunter{position.x, position.y, sight, genome.speed, hungerOf(animal, genome)}, preys,
            [&world](int x, int y) { return tileFactsAt(world, x, y).carcass; },
            mixSeed(static_cast<std::uint64_t>(properties.animalRandomSeed), mixSeed(tick, id)));
        if (choice.kind != HuntChoice::Kind::None) {
            hasTarget = true;
            targetX = choice.x;
            targetY = choice.y;
            kind = choice.kind == HuntChoice::Kind::Carcass ? "carcass" : (choice.atTeeth ? "teeth" : "prey");
        }
    } else if (desire == Desire::Mate) {
        std::vector<MateCandidate> mates;
        for (const auto other : registry.view<const AnimalComponent, const AnimalGenomeComponent,
                                              const DesireComponent, const IdentityComponent,
                                              const PositionComponent>()) {
            const auto& matePosition = registry.get<const PositionComponent>(other);
            mates.push_back(MateCandidate{registry.get<const IdentityComponent>(other).id, matePosition.x,
                                           matePosition.y,
                                           registry.get<const AnimalGenomeComponent>(other).species,
                                           registry.all_of<PredatorComponent>(other),
                                           registry.get<const AnimalComponent>(other).sex,
                                           registry.get<const DesireComponent>(other).current == Desire::Mate});
        }

        const Suitor suitor{id, position.x, position.y, sight, genome.species, predator, animal.sex};
        if (!anyMateInSight(suitor, mates)) {
            // То же решение, что и в AnimalSystem: рядом никого не видно —
            // пробуем зов (hearCall). Он не строит дорогу (звук не
            // спрашивает брода), поэтому и рисуется иначе — прямой линией,
            // а не ломаной по клеткам Reach, см. ниже.
            const MateChoice call = hearCall(suitor, mates);
            if (call.found) {
                hasTarget = true;
                targetX = call.x;
                targetY = call.y;
                kind = "call";
                isCall = true;
            }
        } else {
            const MateChoice choice = chooseMate(reach, suitor, mates);
            if (choice.found) {
                hasTarget = true;
                targetX = choice.x;
                targetY = choice.y;
                kind = "mate";
            }
        }
    }

    if (!hasTarget) {
        pathGroup(0);
        return;
    }
    watched["road_kind"] = kind;
    watched["road_x"] = targetX;
    watched["road_y"] = targetY;

    // Зов — не дорога: цель дальше Reach (звук слышен дальше, чем видно), и
    // roadTo на ней честно вернул бы пустоту — Reach построен только на
    // радиус восприятия. Рисуем то, что животное на самом деле знает: не
    // путь, а прямое направление, одной линией.
    if (isCall) {
        cells.assign(1, PathCell{targetX, targetY});
    } else {
        reach.roadTo(targetX, targetY, cells);
    }
    auto roadJson = nlohmann::json::array();
    for (const auto& cell : cells) {
        roadJson.push_back(cell.x);
        roadJson.push_back(cell.y);
    }
    watched["road"] = std::move(roadJson);
    pathGroup(static_cast<int>(cells.size()));
}

// Геном — по таблице черт своей диеты, а не полем за полем: новая черта
// уедет клиенту сама, как это уже сделано для архетипов видов.
template <typename Traits, typename Genome>
nlohmann::json makeGenomeGroup(const Traits& traits, const Genome& genome) {
    auto pairs = nlohmann::json::array();
    for (const auto& trait : traits) {
        auto pair = nlohmann::json::array();
        pair.push_back(trait.name);
        pair.push_back(genome.*trait.gene);
        pairs.push_back(std::move(pair));
    }
    return nlohmann::json{{"title", "Genome"}, {"values", std::move(pairs)}};
}

} // namespace

nlohmann::json NetworkServer::buildWatchedJson() const {
    WatchTarget target;
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        target = watch_;
    }
    if (target.kind != "animal" && target.kind != "plant") {
        return nlohmann::json{};
    }

    const auto& registry = world_.registry();
    nlohmann::json watched;

    if (target.kind == "animal") {
        // Ищем по идентификатору, а не по клетке: животное ходит, и к
        // моменту сборки сообщения оно уже не там, где по нему кликнули, —
        // в этом и смысл слежения.
        // Гоблина по этому пути искать нельзя: карточка ниже читает
        // DesireComponent, которого у него нет вовсе (см.
        // GoblinDesireComponent). Смотреть за гоблином — своя ветка, и она
        // появится вместе с его протоколом.
        for (const auto entity :
             registry.view<const IdentityComponent, const AnimalComponent>(entt::exclude<GoblinComponent>)) {
            if (registry.get<const IdentityComponent>(entity).id != target.id) {
                continue;
            }
            const auto& animal = registry.get<const AnimalComponent>(entity);
            const auto& genome = registry.get<const AnimalGenomeComponent>(entity);
            const auto& position = registry.get<const PositionComponent>(entity);
            const auto& desire = registry.get<const DesireComponent>(entity);
            const bool predator = registry.all_of<PredatorComponent>(entity);

            watched["kind"] = "animal";
            watched["id"] = target.id;
            watched["x"] = position.x;
            watched["y"] = position.y;
            watched["species"] = genome.species;
            watched["diet"] = predator ? "predator" : "herbivore";
            watched["sex"] = sexName(animal.sex);
            watched["desire"] = desireName(desire.current);

            auto groups = nlohmann::json::array();
            // Хромота (InjuryComponent) — рядом со здоровьем: это тоже
            // состояние тела, просто временное. Через try_get, а не
            // напрямую: наблюдатель не должен падать на звере, которому
            // компонент почему-либо не завели.
            const auto* injury = registry.try_get<const InjuryComponent>(entity);
            groups.push_back(makeGroup("Body", {{"age", animal.age},
                                                 {"growth", animal.growth},
                                                 {"health", animal.health},
                                                 {"lame_ticks", injury != nullptr ? injury->lameTicks : 0},
                                                 {"energy", animal.energy},
                                                 {"water", animal.water},
                                                 {"protein", animal.protein},
                                                 {"dung", animal.dung},
                                                 {"step_progress", animal.stepProgress}}));
            // Голод и жажда не хранятся в мире — они и есть тело, прочитанное
            // с другой стороны (core/Needs.hpp, та же формула, по которой
            // животное выбирает желание). Страха здесь нет: он считается из
            // чужого присутствия, а не из своего тела, и живёт ровно один тик
            // внутри AnimalSystem. Виден он всё равно — в "desire", где стоит
            // "flee", пока животное бежит.
            groups.push_back(makeGroup("Desires", {{"hunger", hungerOf(animal, genome)},
                                                    {"thirst", thirstOf(animal, genome)},
                                                    {"mating", desire.mating}}));
            groups.push_back(makeGenomeGroup(predator ? predatorTraits() : herbivoreTraits(), genome));

            // Дорога — единственное, что видно не только в числах, но и на
            // карте: докуда зверь может дойти и куда сейчас идёт (см.
            // appendRoad).
            appendRoad(world_, entity, animal, genome, position, desire.current, predator, target.id, watched,
                       groups);

            watched["groups"] = std::move(groups);
            return watched;
        }
        // Не нашлось — животное умерло или было съедено, пока за ним
        // следили. Это тоже ответ, и клиенту важно его получить: иначе он
        // показывал бы последнее известное состояние как текущее.
        return nlohmann::json{{"kind", "gone"}};
    }

    if (!world_.area().inBounds(target.x, target.y)) {
        return nlohmann::json{{"kind", "gone"}};
    }
    // Растение ищется по клетке (на тайле оно одно) и через индекс
    // размещения Area, а не обходом всех растений мира: их тысячи, а
    // сообщение собирается по нескольку раз в секунду.
    for (const auto entity : world_.area().cellAt(target.x, target.y).entities) {
        if (!registry.all_of<PlantComponent, PlantGenomeComponent>(entity)) {
            continue;
        }
        const auto& plant = registry.get<const PlantComponent>(entity);
        const auto& genome = registry.get<const PlantGenomeComponent>(entity);

        watched["kind"] = "plant";
        watched["x"] = target.x;
        watched["y"] = target.y;
        watched["species"] = genome.species;

        auto groups = nlohmann::json::array();
        groups.push_back(makeGroup("Body", {{"age", plant.age},
                                             {"growth", plant.growth},
                                             {"minerals", plant.minerals},
                                             {"stress", plant.stress}}));
        groups.push_back(makeGenomeGroup(kGrassTraits, genome));
        watched["groups"] = std::move(groups);
        return watched;
    }
    return nlohmann::json{{"kind", "gone"}};
}

std::string NetworkServer::buildInitMessage(const LayerSnapshot& layers, const nlohmann::json& watched) const {
    nlohmann::json message;
    message["type"] = "world_init";
    message["area"]["width"] = layers.width;
    message["area"]["height"] = layers.height;
    message["paused"] = paused_.load();
    message["tick"] = world_.registry().get<const TimeComponent>(world_.worldEntity()).tick;

    {
        std::lock_guard<std::mutex> lock(generationConfigMutex_);
        message["world"] = currentWorldName_;
        // Все параметры генерации — одним объектом, тем же самым, что
        // лежит в файле мира: панель настроек клиента строится из этого
        // сообщения целиком, без вкомпилированных умолчаний. Полями по
        // секциям они лежать больше не могут — имя настройки столкнулось
        // бы с именем состояния мира ("animals" хотят и те, и другие), и
        // одно молча затёрло бы другое.
        message["generation"] = currentGenerationConfig_;
    }

    // Константы законов мира (core/Diagnostics.hpp) — только для показа.
    // Едут в world_init, а не отдельным сообщением: они не меняются
    // никогда, а world_init по определению содержит всё, из чего клиент
    // строит картину мира с нуля. Шесть десятков коротких записей рядом с
    // плотными массивами на всю Область ничего не весят.
    auto constants = nlohmann::json::array();
    for (const auto& constant : coreConstants()) {
        constants.push_back({{"group", constant.group}, {"name", constant.name}, {"value", constant.value}});
    }
    message["constants"] = constants;

    // Булыжники и источники — разреженно, тегом без данных: их немного
    // (boulder_count + river_count + water_source_count), плотный массив
    // на всю Область был бы избыточен. Меняются только генерацией,
    // поэтому в дельты не входят.
    auto boulders = nlohmann::json::array();
    world_.registry()
        .view<const ImpassableComponent, const PositionComponent>()
        .each([&](auto /*entity*/, const PositionComponent& pos) {
            boulders.push_back({{"x", pos.x}, {"y", pos.y}});
        });
    message["boulders"] = boulders;

    auto waterSources = nlohmann::json::array();
    world_.registry()
        .view<const WaterSourceComponent, const PositionComponent>()
        .each([&](auto /*entity*/, const PositionComponent& pos) {
            waterSources.push_back({{"x", pos.x}, {"y", pos.y}});
        });
    message["water_sources"] = waterSources;

    // Виды травы этого мира (их не больше kMaxGrassSpecies): клиенту
    // нужны и цвет по индексу, и сами числа — чтобы можно было
    // посмотреть, какая стратегия у травы под курсором. Поля генома
    // перечисляет таблица черт, а не этот код: новая черта уедет клиенту
    // сама. Без округления, в отличие от плотных массивов: видов не
    // больше двенадцати, на трафик это не влияет, а часть черт (расход
    // влаги за тик) живёт в тысячных долях и от округления превратилась
    // бы в ноль.
    auto speciesJson = nlohmann::json::array();
    const auto& plantSpecies = world_.registry().get<const PlantSpeciesComponent>(world_.worldEntity());
    for (const auto& archetype : plantSpecies.grasses) {
        nlohmann::json record;
        record["species"] = archetype.species;
        for (const auto& trait : kGrassTraits) {
            record[trait.name] = archetype.*trait.gene;
        }
        speciesJson.push_back(std::move(record));
    }
    message["plant_species"] = speciesJson;

    // Виды деревьев — отдельным списком по той же причине, что и слой:
    // нумерация у них своя, и таблица черт своя (kTreeTraits).
    auto treeSpeciesJson = nlohmann::json::array();
    for (const auto& archetype : plantSpecies.trees) {
        nlohmann::json record;
        record["species"] = archetype.species;
        for (const auto& trait : kTreeTraits) {
            record[trait.name] = archetype.*trait.gene;
        }
        treeSpeciesJson.push_back(std::move(record));
    }
    message["tree_species"] = treeSpeciesJson;

    // Виды животных — по тем же правилам, что и виды травы: клиенту нужны
    // и цвет по индексу, и сами числа, а перечисляет их таблица черт, а не
    // этот код. Списка два, потому что и таблицы черт две.
    const auto& animalSpecies = world_.registry().get<const AnimalSpeciesComponent>(world_.worldEntity());
    auto speciesToJson = [](const std::vector<AnimalGenomeComponent>& archetypes,
                             std::span<const AnimalTrait> traits) {
        auto array = nlohmann::json::array();
        for (const auto& archetype : archetypes) {
            nlohmann::json record;
            record["species"] = archetype.species;
            for (const auto& trait : traits) {
                record[trait.name] = archetype.*trait.gene;
            }
            array.push_back(std::move(record));
        }
        return array;
    };
    message["animal_species"] = {{"herbivores", speciesToJson(animalSpecies.herbivores, herbivoreTraits())},
                                  {"predators", speciesToJson(animalSpecies.predators, predatorTraits())}};

    // Сгенерирован ли мир вообще. Сервер поднимается с пустой Областью и
    // ждёт, пока мир создадут с панели генерации, — и клиент должен
    // отличать "мир, в котором ничего нет" от "мира ещё нет": в первом
    // случае показывать пустую карту правильно, во втором она сбивает с
    // толку.
    message["generated"] = !world_.registry().view<const PositionComponent>().empty();

    message["animals"] = animalsToJson(layers.animals);

    // Подробности выбранного — в world_init тоже: подключившийся (или
    // переподключившийся) клиент должен увидеть открытую карточку
    // существа сразу, а не после первого его шага.
    if (!watched.is_null()) {
        message["watched"] = watched;
    }

    // Летопись численности — целиком: world_init по определению содержит
    // всё, из чего клиент строит картину мира с нуля, а прошлое мира —
    // такая же его часть, как текущая карта. Клиент заменяет ею свою
    // ("full"), а не дописывает: мир мог смениться (регенерация,
    // загрузка), и сшивать две летописи было бы враньём.
    auto history = history_.toJson();
    history["full"] = true;
    // Черты, по которым записан средний геном в точках летописи: имя и
    // границы вложения (atZero — худшее для существа значение черты,
    // atOne — лучшее, см. core/generation/Genetics.hpp). Порядок тот же,
    // в каком гены лежат в самой точке.
    //
    // Границы уходят вместе с именем не для полноты: по ним клиент кладёт
    // все черты на одну шкалу "вложения" 0..1, и разные по величине гены
    // (возраст в тысячах тиков и скорость в тысячных клетки) становятся
    // сравнимы между собой. Считать эту нормировку на сервере значило бы
    // потерять сами значения, а клиенту нужны и они — в подписи.
    //
    // Один раз, в world_init: таблица черт не меняется за жизнь мира
    // вовсе, она вкомпилирована в ядро.
    // Таблица берётся по ссылке (auto&&): kGrassTraits — сырой массив, и
    // приём по значению превратил бы его в указатель, по которому уже не
    // пройтись.
    const auto traitsToJson = [](auto&& traits) {
        auto array = nlohmann::json::array();
        for (const auto& trait : traits) {
            array.push_back({{"name", trait.name}, {"lo", trait.atZero}, {"hi", trait.atOne}});
        }
        return array;
    };
    history["traits"] = {{"plants", traitsToJson(kGrassTraits)},
                          {"trees", traitsToJson(kTreeTraits)},
                          {"herbivores", traitsToJson(herbivoreTraits())},
                          {"predators", traitsToJson(predatorTraits())}};
    message["history"] = std::move(history);

    message["layers"]["rockiness"] = layers.rockiness;
    message["layers"]["moisture"] = layers.moisture;
    message["layers"]["minerals"] = layers.minerals;
    message["layers"]["height"] = layers.terrainHeight;
    message["layers"]["water"] = layers.water;
    message["layers"]["humus"] = layers.humus;
    message["layers"]["carcass"] = layers.carcass;
    message["layers"]["species"] = layers.species;
    message["layers"]["growth"] = layers.growth;
    message["layers"]["seeds"] = layers.seeds;
    message["layers"]["trees"] = layers.trees;

    return message.dump();
}

std::string NetworkServer::buildDeltaMessage(const LayerSnapshot& previous, const LayerSnapshot& current,
                                              const nlohmann::json& watched, bool watchedChanged) const {
    nlohmann::json message;
    bool anyChange = false;

    // Выбранное существо — единственная часть дельты, которая взводит
    // anyChange сама: на паузе мир не меняется вовсе, но клик по другому
    // зверю должен показать его карточку, не дожидаясь снятия паузы.
    if (watchedChanged) {
        message["watched"] = watched.is_null() ? nlohmann::json{{"kind", "none"}} : watched;
        anyChange = true;
    }

    // Каменистость сюда не входит: её меняет только генерация, а она
    // рассылает новый world_init.
    const std::pair<const char*, std::pair<const std::vector<int>*, const std::vector<int>*>> layers[] = {
        {"moisture", {&previous.moisture, &current.moisture}},
        {"minerals", {&previous.minerals, &current.minerals}},
        {"height", {&previous.terrainHeight, &current.terrainHeight}},
        {"water", {&previous.water, &current.water}},
        {"humus", {&previous.humus, &current.humus}},
        {"carcass", {&previous.carcass, &current.carcass}},
        {"species", {&previous.species, &current.species}},
        {"growth", {&previous.growth, &current.growth}},
        {"seeds", {&previous.seeds, &current.seeds}},
        {"trees", {&previous.trees, &current.trees}},
    };
    for (const auto& [name, arrays] : layers) {
        auto pairs = changedCells(*arrays.first, *arrays.second);
        if (!pairs.empty()) {
            anyChange = true;
            message[name] = std::move(pairs);
        }
    }

    // Животные — изменениями, как и слои (см. протокол в
    // NetworkServer.hpp): ушедшие и правки по индексам в прежнем списке,
    // родившиеся — полными карточками.
    auto animals = animalsDeltaJson(previous.animals, current.animals);
    if (!animals.is_null()) {
        anyChange = true;
        message["animals"] = std::move(animals);
    }

    // Летопись — только точки новее отправленных. Прореживание меняет все
    // точки разом (см. PopulationHistory::thin), и узнаётся оно по
    // изменившемуся шагу: тогда клиенту уходит вся летопись целиком.
    // Новых точек без нового тика не бывает (точка пишется по завершении
    // тика, до рассылки), поэтому сама по себе летопись поводом отправить
    // дельту не считается — anyChange она не взводит.
    const bool historyFull = !sentHistoryValid_ || history_.interval() != sentHistoryInterval_;
    auto history = historyFull ? history_.toJson() : history_.toJson(sentHistoryTick_);
    if (historyFull || !history["points"].empty()) {
        history["full"] = historyFull;
        message["history"] = std::move(history);
    }

    const auto tick = world_.registry().get<const TimeComponent>(world_.worldEntity()).tick;
    const bool paused = paused_.load();
    if (!anyChange && tick == sentTick_ && paused == sentPaused_) {
        // Не изменилось вообще ничего (обычное дело на паузе) — молчим.
        return {};
    }

    message["type"] = "world_delta";
    message["tick"] = tick;
    message["paused"] = paused;
    return message.dump();
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

void NetworkServer::handleClientMessage(const std::string& payload, const ix::WebSocket* sender) {
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
    if (type == "updates") {
        // "Мне сейчас не показывай". Свёрнутое окно, окно без фокуса,
        // нажатая клавиша — что именно, решает клиент; сервер только
        // перестаёт слать ЭТОМУ клиенту состояние мира. Мир при этом
        // продолжает жить: отвернуться — не то же самое, что поставить на
        // паузу (для паузы есть toggle_pause).
        const bool enabled = json.value("enabled", true);
        {
            std::lock_guard<std::mutex> lock(suspendedMutex_);
            if (enabled) {
                suspended_.erase(sender);
            } else {
                suspended_.insert(sender);
            }
        }
        if (enabled) {
            // Вернувшемуся — мир целиком: пока он не смотрел, дельты
            // уходили без него (или не собирались вовсе), и накладывать
            // новую дельту не на что.
            requestFullResync();
        }
        return;
    }
    if (type == "toggle_pause") {
        const bool newState = !paused_.load();
        pauseCommandCount_.fetch_add(1);
        paused_.store(newState);
        std::cout << "World " << (newState ? "paused" : "resumed") << " by client request.\n";
        broadcastPauseState();
    } else if (type == "watch") {
        // Только запоминаем выбор — ECS registry отсюда (сетевой поток)
        // трогать нельзя, подробности соберёт publish на потоке GameLoop.
        WatchTarget target;
        target.kind = json.value("kind", std::string{"none"});
        target.id = json.value("id", static_cast<std::uint64_t>(0));
        target.x = json.value("x", 0);
        target.y = json.value("y", 0);
        std::lock_guard<std::mutex> lock(watchMutex_);
        watch_ = target;
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
    } else if (type == "delete_world") {
        // Только файловый ввод-вывод, ECS registry не трогаем — можно
        // прямо здесь, на сетевом потоке, как list_worlds.
        const std::string name = json.value("name", std::string{});
        std::string error;
        if (deleteWorld(name, savesDirectory_, error)) {
            std::cout << "World '" << name << "' deleted by client request.\n";
            broadcastNotice("info", "World '" + name + "' deleted.");
            broadcastWorldList();
        } else {
            std::cerr << "NetworkServer: delete_world failed (" << error << ")\n";
            broadcastNotice("error", error);
        }
    } else if (type == "save_generation_config") {
        // Только файловый ввод-вывод, ECS registry не трогаем — можно
        // прямо здесь, на сетевом потоке, как и toggle_pause.
        //
        // Сохраняем то, что прислал клиент (набранное на панели), а не
        // currentGenerationConfig_: последний обновляется только после
        // успешной регенерации, и раньше "Save values" без предварительного
        // "Regenerate" молча записывал в файл прежние значения — правки
        // ползунков не сохранялись вовсе. currentGenerationConfig_ при этом
        // не трогаем: он описывает, чем сгенерирован мир, лежащий в памяти,
        // и уходит и в world_init, и в файл сохранённого мира — подменять
        // его непримененными правками значило бы врать о содержимом мира.
        RegenerationRequest toWrite;
        if (json.contains("params")) {
            try {
                toWrite = json.at("params").get<RegenerationRequest>();
            } catch (const nlohmann::json::exception& e) {
                std::cerr << "NetworkServer: invalid save_generation_config request (" << e.what() << ")\n";
                return;
            }
        } else {
            std::lock_guard<std::mutex> lock(generationConfigMutex_);
            toWrite = currentGenerationConfig_;
        }

        // Размер Области попадает в config.json вместе с остальным: он
        // теперь такой же параметр генерации, как seed, и "Save values"
        // должна делать умолчанием при следующем запуске именно то, что
        // набрано на панели.
        //
        // Раскладывает поля applyGeneration рядом со структурами
        // (shared/config/Config.hpp), а не список присваиваний здесь: пока
        // список был здесь, в нём не хватало животных, и правки поголовья
        // "Save values" молча выбрасывала.
        ServerConfig toSave = baseConfig_;
        applyGeneration(toSave, toWrite);
        saveServerConfig(configPath_, toSave);
        std::cout << "Generation config saved to '" << configPath_ << "'.\n";
        broadcastNotice("info", "Generation values saved to config.");
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

void NetworkServer::broadcastSnapshot(const std::string& payload) {
    std::lock_guard<std::mutex> lock(suspendedMutex_);
    for (const auto& client : server_.getClients()) {
        if (suspended_.count(client.get()) == 0) {
            client->send(payload);
        }
    }
}

bool NetworkServer::anyoneWatching() {
    std::lock_guard<std::mutex> lock(suspendedMutex_);
    for (const auto& client : server_.getClients()) {
        if (suspended_.count(client.get()) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace goblins

#include "server/NetworkServer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

#include "core/Needs.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HeightComponent.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SeedComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"
#include "server/WorldSave.hpp"

namespace goblins {

namespace {

// Делитель для слоёв, которые на сервере хранятся долями (влажность,
// плотность, каменистость, глубина воды, высота). По сети они уходят
// целыми в тысячных долях: в JSON число с плавающей точкой печатается
// как double — два десятка знаков на значение, — а массив тут плотный,
// на всю Область. Тысячных хватает: клиент по этим числам выбирает
// оттенок тайла.
constexpr int kMilliScale = 1000;

// Порог невыбранной очереди отправки у клиента, при котором очередная
// рассылка пропускается: клиент не успевает принимать, и копить для него
// данные бессмысленно. Изменения не теряются — следующая дельта считается
// от последнего реально отправленного состояния и включит в себя
// пропущенное. Не настройка: это свойство транспорта, а не мира, и от
// мира к миру оно не меняется.
constexpr std::size_t kSnapshotBacklogBytes = 1024 * 1024;

int encodeMilli(float value) {
    return static_cast<int>(std::lround(static_cast<double>(value) * kMilliScale));
}

// Развитость растения — целые проценты, а не доля: значение нужно
// клиенту только чтобы выбрать насыщенность цвета тайла.
int growthPercent(float growth) {
    return static_cast<int>(std::lround(std::clamp(growth, 0.0f, 1.0f) * 100.0f));
}

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

void NetworkServer::requestFullResync() {
    needsFullResync_.store(true);
}

void NetworkServer::publish(bool force) {
    if (server_.getClients().empty()) {
        // Мир существует независимо от наблюдателя (02_CorePrinciples.md,
        // п.1) и продолжает тикать — но сериализовать его сейчас некому,
        // а это самая дорогая часть тика. Точка отсчёта для дельт при
        // этом протухает, поэтому следующий подключившийся клиент
        // получит полный world_init (его же запрашивает и колбэк Open).
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

    broadcastToAll(payload);

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
            out.moisture[i] = encodeMilli(soil.moisture);
            out.rockiness[i] = encodeMilli(soil.rockiness);
            out.minerals[i] = soil.minerals;
        });

    // Высота рельефа (HeightComponent всегда идёт в паре с
    // SoilComponent на террейн-Entity, см. WorldSave.hpp) — нужна
    // клиенту для необязательного слоя-рельефа; единиц измерения не
    // несёт, клиент нормализует по min/max текущей карты.
    registry.view<const PositionComponent, const HeightComponent>().each(
        [&](const PositionComponent& pos, const HeightComponent& h) {
            out.terrainHeight[static_cast<std::size_t>(pos.y) * width + pos.x] = encodeMilli(h.height);
        });

    // Вода и перегной на сервере разреженны (нет компонента = нет
    // возможности, 02_CorePrinciples.md, п.3), но по сети идут плотными
    // массивами, как и почва: ноль значит "нет". Так дельта одинаково
    // выражает и появление воды, и её уход.
    registry.view<const PositionComponent, const WaterComponent>().each(
        [&](const PositionComponent& pos, const WaterComponent& w) {
            out.water[static_cast<std::size_t>(pos.y) * width + pos.x] = encodeMilli(w.depth);
        });

    registry.view<const PositionComponent, const HumusComponent>().each(
        [&](const PositionComponent& pos, const HumusComponent& tileHumus) {
            out.humus[static_cast<std::size_t>(pos.y) * width + pos.x] = tileHumus.minerals;
        });

    // Падаль — такое же состояние тайла, как перегной, и уходит таким же
    // плотным слоем: ноль значит "туши здесь нет".
    registry.view<const PositionComponent, const CarcassComponent>().each(
        [&](const PositionComponent& pos, const CarcassComponent& tileCarcass) {
            out.carcass[static_cast<std::size_t>(pos.y) * width + pos.x] = encodeMilli(tileCarcass.meat);
        });

    registry.view<const PositionComponent, const PlantComponent, const PlantGenomeComponent>().each(
        [&](const PositionComponent& pos, const PlantComponent& plant, const PlantGenomeComponent& genome) {
            const std::size_t i = static_cast<std::size_t>(pos.y) * width + pos.x;
            out.species[i] = genome.species;
            out.growth[i] = growthPercent(plant.growth);
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
            out.animals.push_back(LayerSnapshot::AnimalView{identity != nullptr ? identity->id : 0, pos.x, pos.y,
                                                            genome.species, growthPercent(animal.growth),
                                                            growthPercent(animal.health),
                                                            registry.all_of<PredatorComponent>(entity), animal.sex,
                                                            desire.current});
        });
    std::sort(out.animals.begin(), out.animals.end(),
              [](const LayerSnapshot::AnimalView& a, const LayerSnapshot::AnimalView& b) {
                  if (a.y != b.y) return a.y < b.y;
                  if (a.x != b.x) return a.x < b.x;
                  if (a.predator != b.predator) return a.predator < b.predator;
                  if (a.species != b.species) return a.species < b.species;
                  if (a.sex != b.sex) return a.sex < b.sex;
                  if (a.desire != b.desire) return a.desire < b.desire;
                  if (a.growth != b.growth) return a.growth < b.growth;
                  if (a.health != b.health) return a.health < b.health;
                  // Двух неразличимых снаружи животных на одной клетке
                  // порядок всё равно обязан расставить одинаково от тика к
                  // тику, иначе дельта видела бы изменение там, где мир не
                  // менялся; постоянный идентификатор — единственное, чем
                  // они точно отличаются.
                  return a.id < b.id;
              });
}

nlohmann::json NetworkServer::animalsToJson(const std::vector<LayerSnapshot::AnimalView>& animals) {
    auto array = nlohmann::json::array();
    for (const auto& animal : animals) {
        array.push_back({{"id", animal.id},
                          {"x", animal.x},
                          {"y", animal.y},
                          {"species", animal.species},
                          {"kind", animal.predator ? "predator" : "herbivore"},
                          {"growth", animal.growth},
                          {"health", animal.health},
                          {"sex", sexName(animal.sex)},
                          {"desire", desireName(animal.desire)}});
    }
    return array;
}

namespace {

// Группа значений в "watched": заголовок и пары "имя-число" в осмысленном
// порядке (тем же, в котором они перечислены в таблице черт или в самом
// компоненте). Массив пар, а не объект: у объекта порядок ключей теряется,
// а читать геном вразнобой — совсем не то же самое, что по таблице.
nlohmann::json makeGroup(const char* title, std::initializer_list<std::pair<const char*, float>> values) {
    auto pairs = nlohmann::json::array();
    for (const auto& [name, value] : values) {
        auto pair = nlohmann::json::array();
        pair.push_back(name);
        pair.push_back(value);
        pairs.push_back(std::move(pair));
    }
    return nlohmann::json{{"title", title}, {"values", std::move(pairs)}};
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
        for (const auto entity : registry.view<const IdentityComponent, const AnimalComponent>()) {
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
            groups.push_back(makeGroup("Body", {{"age", animal.age},
                                                 {"growth", animal.growth},
                                                 {"health", animal.health},
                                                 {"energy", animal.energy},
                                                 {"water", animal.water},
                                                 {"protein", static_cast<float>(animal.protein)},
                                                 {"dung", static_cast<float>(animal.dung)},
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
                                             {"minerals", static_cast<float>(plant.minerals)},
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
    message["scale"] = kMilliScale;
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
    for (const auto& archetype :
         world_.registry().get<const PlantSpeciesComponent>(world_.worldEntity()).archetypes) {
        nlohmann::json record;
        record["species"] = archetype.species;
        for (const auto& trait : kGrassTraits) {
            record[trait.name] = archetype.*trait.gene;
        }
        speciesJson.push_back(std::move(record));
    }
    message["plant_species"] = speciesJson;

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
    };
    for (const auto& [name, arrays] : layers) {
        auto pairs = changedCells(*arrays.first, *arrays.second);
        if (!pairs.empty()) {
            anyChange = true;
            message[name] = std::move(pairs);
        }
    }

    // Список животных — целиком и только если изменился (см. протокол в
    // NetworkServer.hpp): при десятках животных это дешевле, чем описывать
    // перемещения ключами, а не изменился он ровно тогда, когда стадо
    // стояло на месте.
    if (previous.animals != current.animals) {
        anyChange = true;
        message["animals"] = animalsToJson(current.animals);
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

        ServerConfig toSave = baseConfig_;
        // Размер Области попадает в config.json вместе с остальным: он
        // теперь такой же параметр генерации, как seed, и "Save values"
        // должна делать умолчанием при следующем запуске именно то, что
        // набрано на панели.
        toSave.area.width = std::clamp(toWrite.area_width, kMinAreaSide, kMaxAreaSide);
        toSave.area.height = std::clamp(toWrite.area_height, kMinAreaSide, kMaxAreaSide);
        toSave.seed = toWrite.seed;
        toSave.terrain = toWrite.terrain;
        toSave.boulder_count = toWrite.boulder_count;
        toSave.plants = toWrite.plants;
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

} // namespace goblins

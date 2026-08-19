#include "server/WorldSave.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HeightComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/InjuryComponent.hpp"
#include "core/components/MovementComponent.hpp"
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
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "platform/ExecutablePath.hpp"

namespace goblins {

namespace {

constexpr const char* kFormatTag = "goblins_world";
constexpr const char* kExtension = ".json";

std::string formatUtcNow(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), format, &utc);
    return buffer;
}

std::string utcTimestamp() {
    return formatUtcNow("%Y-%m-%dT%H:%M:%SZ");
}

std::filesystem::path savePath(const std::filesystem::path& directory, const std::string& name) {
    return directory / (name + kExtension);
}

// Геном пишется и читается обходом таблицы черт (core::kGrassTraits для
// травы, core::kHerbivoreTraits для травоядных), а не перечислением полей:
// имена в файле — это имена черт, поэтому новая черта попадает в
// сохранение сама. Отсутствующая в старом файле черта берёт значение по
// умолчанию — та же логика, что и у "height"/"minerals" (см. заголовок
// WorldSave.hpp): формат от этого не ломается и версию поднимать не нужно.
//
// Функции шаблонные по типу генома и его таблице: механика "геном — это
// таблица черт" общая для всего живого (core/generation/Genetics.hpp), и
// сериализация обязана быть общей ровно по той же причине — иначе у
// второго существа завелась бы вторая, отдельно живущая копия того же
// кода.
template <typename Genome, std::size_t N>
nlohmann::json genomeToJson(const Genome& genome, const genetics::Trait<Genome> (&traits)[N]) {
    nlohmann::json record;
    record["species"] = genome.species;
    for (const auto& trait : traits) {
        record[trait.name] = genome.*trait.gene;
    }
    return record;
}

template <typename Genome, std::size_t N>
Genome genomeFromJson(const nlohmann::json& record, const genetics::Trait<Genome> (&traits)[N]) {
    Genome genome;
    genome.species = record.value("species", 0);
    for (const auto& trait : traits) {
        genome.*trait.gene = record.value(trait.name, genome.*trait.gene);
    }
    return genome;
}

// Разобранный Entity из файла — промежуточный шаг между JSON и ECS.
// Нужен именно как отдельный тип: сначала разбирается и проверяется весь
// файл целиком и только потом мир сбрасывается и заполняется, иначе
// ошибка на середине файла оставила бы мир полузагруженным.
struct ParsedEntity {
    bool hasPosition = false;
    PositionComponent position{};
    bool hasSoil = false;
    SoilComponent soil{};
    // Не разделяем на hasHeight: высота всегда идёт в паре с почвой
    // (parsed.hasSoil), а для сохранений без поля "height" уже есть
    // безопасное значение по умолчанию — 0 (плоский рельеф).
    int height = 0;
    bool hasWater = false;
    WaterComponent water{};
    bool impassable = false;
    // Тег, как impassable — сам факт наличия и есть данные.
    bool waterSource = false;
    // World Entity — это Entity с TimeComponent и WorldPropertiesComponent
    // (06_GameLoop.md, п.1/п.1a). При загрузке он не создаётся заново:
    // World::reset уже создал его (со значениями по умолчанию для
    // свойств мира) — применяются только сохранённые значения.
    bool hasTime = false;
    std::uint64_t tick = 0;
    bool hasWorldProperties = false;
    WorldPropertiesComponent worldProperties{};
    // Виды травы — тоже данные World Entity (PlantSpeciesComponent, см.
    // 06_GameLoop.md, п.1a): выбраны при генерации и не меняются. Виды
    // травоядных живут там же и по тем же правилам.
    bool hasPlantSpecies = false;
    std::vector<PlantGenomeComponent> plantSpecies;
    bool hasAnimalSpecies = false;
    AnimalSpeciesComponent animalSpecies{};

    // Живое растение — Entity с состоянием и геномом (оба обязательно
    // вместе: растение без генома не смогло бы ни расти, ни дать потомка).
    bool hasPlant = false;
    PlantComponent plant{};
    PlantGenomeComponent genome{};

    // Лежащее в клетке семя (SeedComponent) — тот же геном, но без
    // состояния растения: оно ещё не проросло. Растением и семенем
    // одновременно Entity быть не может, поэтому геном у них общий —
    // parsed.genome выше.
    bool hasSeed = false;
    SeedComponent seed{};

    // Живое животное — Entity с телом, геномом, желаниями, постоянным
    // идентификатором и тегом диеты. Всё это обязательно вместе: без
    // генома животное не смогло бы ни расти, ни принести потомство, без
    // идентификатора у него не было бы ключа случайности (см.
    // IdentityComponent), желания — то, чем оно занято прямо сейчас, а без
    // диеты оно не знало бы, что для него еда.
    bool hasAnimal = false;
    bool predator = false;
    AnimalComponent animal{};
    AnimalGenomeComponent animalGenome{};
    DesireComponent desire{};
    std::uint64_t identity = 0;

    // Падаль лежит на терраформирующем Entity тайла, рядом с почвой,
    // водой и перегноем (см. CarcassComponent).
    bool hasCarcass = false;
    CarcassComponent carcass{};

    // Перегной лежит на терраформирующем Entity тайла, рядом с почвой и
    // водой (см. HumusComponent), поэтому это признак того же самого
    // Entity, а не отдельная запись.
    bool hasHumus = false;
    HumusComponent humus{};
};

nlohmann::json buildEntitiesJson(const World& world) {
    const auto& registry = world.registry();
    auto entities = nlohmann::json::array();

    for (const auto entity : registry.view<entt::entity>()) {
        nlohmann::json record = nlohmann::json::object();

        if (const auto* time = registry.try_get<TimeComponent>(entity)) {
            record["time"] = {{"tick", time->tick}};
        }
        if (const auto* worldProperties = registry.try_get<WorldPropertiesComponent>(entity)) {
            record["world_properties"] = {{"water_source_depth", worldProperties->waterSourceDepth},
                                          {"water_evaporation_rate", worldProperties->waterEvaporationRate},
                                          {"rain_interval_ticks", worldProperties->rainIntervalTicks},
                                          {"rain_amount", worldProperties->rainAmount},
                                          {"soil_erosion_rate", worldProperties->soilErosionRate},
                                          {"minerals_spread_enabled", worldProperties->toggles.mineralsSpread},
                                          {"erosion_deposition_enabled", worldProperties->toggles.erosionDeposition},
                                          {"plant_mutation_rate", worldProperties->plantMutationRate},
                                          {"humus_decay_period", worldProperties->humusDecayPeriod},
                                          {"plant_random_seed", worldProperties->plantRandomSeed},
                                          {"animal_mutation_rate", worldProperties->animalMutationRate},
                                          {"animal_random_seed", worldProperties->animalRandomSeed}};
        }
        if (const auto* plantSpecies = registry.try_get<PlantSpeciesComponent>(entity)) {
            auto archetypes = nlohmann::json::array();
            for (const auto& archetype : plantSpecies->archetypes) {
                archetypes.push_back(genomeToJson(archetype, kGrassTraits));
            }
            record["plant_species"] = std::move(archetypes);
        }
        if (const auto* animalSpecies = registry.try_get<AnimalSpeciesComponent>(entity)) {
            auto herbivores = nlohmann::json::array();
            for (const auto& archetype : animalSpecies->herbivores) {
                herbivores.push_back(genomeToJson(archetype, kHerbivoreTraits));
            }
            auto predators = nlohmann::json::array();
            for (const auto& archetype : animalSpecies->predators) {
                predators.push_back(genomeToJson(archetype, kPredatorTraits));
            }
            // Два списка под одним ключом, потому что и в мире это один
            // компонент: у травоядных и хищников свои таблицы черт, но
            // виды и тех, и других — одно свойство мира.
            record["animal_species"] = {{"herbivores", std::move(herbivores)},
                                         {"predators", std::move(predators)}};
        }
        if (const auto* position = registry.try_get<PositionComponent>(entity)) {
            record["position"] = {{"x", position->x}, {"y", position->y}};
        }
        if (const auto* soil = registry.try_get<SoilComponent>(entity)) {
            record["soil"] = {{"moisture", soil->moisture},
                              {"rockiness", soil->rockiness},
                              {"minerals", soil->minerals}};
        }
        if (const auto* heightComponent = registry.try_get<HeightComponent>(entity)) {
            record["height"] = heightComponent->height;
        }
        if (const auto* water = registry.try_get<WaterComponent>(entity)) {
            // Остаток разлива (flow_remainder) — такое же состояние мира,
            // как и сама глубина: он несёт неделящуюся часть воды до
            // следующего тика (см. WaterComponent), и потерять его на
            // сохранении значило бы потерять эту воду.
            record["water"] = {{"depth", water->depth}, {"flow_remainder", water->flowRemainder}};
        }
        if (const auto* humus = registry.try_get<HumusComponent>(entity)) {
            record["humus"] = {{"minerals", humus->minerals}};
        }
        if (const auto* plant = registry.try_get<PlantComponent>(entity)) {
            record["plant"] = {{"age", plant->age},
                               {"growth", plant->growth},
                               {"minerals", plant->minerals},
                               {"stress", plant->stress}};
        }
        // Лежащее семя — не растение (см. SeedComponent), но геном у него
        // такой же, и пишется он общей веткой ниже: "genome" есть и у
        // растения, и у семени.
        if (const auto* seed = registry.try_get<SeedComponent>(entity)) {
            record["seed"] = {{"age", seed->age}, {"minerals", seed->minerals}};
        }
        if (const auto* genome = registry.try_get<PlantGenomeComponent>(entity)) {
            record["genome"] = genomeToJson(*genome, kGrassTraits);
        }
        if (const auto* animal = registry.try_get<AnimalComponent>(entity)) {
            record["animal"] = {{"age", animal->age},
                                 {"growth", animal->growth},
                                 {"sex", sexName(animal->sex)},
                                 {"energy", animal->energy},
                                 {"water", animal->water},
                                 {"protein", animal->protein},
                                 {"dung", animal->dung},
                                 {"health", animal->health},
                                 {"step_progress", animal->stepProgress}};
        }
        if (const auto* genome = registry.try_get<AnimalGenomeComponent>(entity)) {
            // Черты пишутся по таблице своей диеты: у хищника их на одну
            // больше, и вписывать травоядному чужие поля было бы ложью о
            // том, из чего он состоит.
            record["animal_genome"] = registry.all_of<PredatorComponent>(entity)
                                           ? genomeToJson(*genome, kPredatorTraits)
                                           : genomeToJson(*genome, kHerbivoreTraits);
        }
        if (const auto* carcass = registry.try_get<CarcassComponent>(entity)) {
            record["carcass"] = {{"meat", carcass->meat}, {"protein", carcass->protein}};
        }
        if (const auto* desire = registry.try_get<DesireComponent>(entity)) {
            // Голод и жажда в файл не пишутся: они пересчитываются из тела
            // на первом же тике и между тиками не живут (см.
            // DesireComponent). Записывать их значило бы сохранять то, что
            // всё равно будет переписано.
            record["desire"] = {{"mating", desire->mating}, {"current", desireName(desire->current)}};
        }
        if (const auto* identity = registry.try_get<IdentityComponent>(entity)) {
            record["identity"] = identity->id;
        }
        // Тег-компонент без данных — в файле это просто признак наличия
        // (02_CorePrinciples.md, п.3: отсутствие компонента = отсутствие
        // возможности, поэтому "impassable": false не пишется вообще).
        if (registry.all_of<ImpassableComponent>(entity)) {
            record["impassable"] = true;
        }
        if (registry.all_of<WaterSourceComponent>(entity)) {
            record["water_source"] = true;
        }
        // Диета — такой же тег без данных, как impassable: сам факт
        // наличия и есть данные.
        if (registry.all_of<HerbivoreComponent>(entity)) {
            record["herbivore"] = true;
        }
        if (registry.all_of<PredatorComponent>(entity)) {
            record["predator"] = true;
        }

        entities.push_back(std::move(record));
    }

    return entities;
}

// Разбирает массив entities. Возвращает false и заполняет outError на
// первой же несостыковке — битое сохранение лучше не открыть вовсе, чем
// открыть наполовину.
bool parseEntities(const nlohmann::json& json, int width, int height, std::vector<ParsedEntity>& outEntities,
                   std::string& outError) {
    if (!json.is_array()) {
        outError = "'entities' is not an array";
        return false;
    }

    outEntities.reserve(json.size());
    for (const auto& record : json) {
        if (!record.is_object()) {
            outError = "entity record is not an object";
            return false;
        }

        ParsedEntity parsed;

        if (record.contains("time")) {
            parsed.hasTime = true;
            parsed.tick = record["time"].value("tick", static_cast<std::uint64_t>(0));
        }
        if (record.contains("world_properties")) {
            parsed.hasWorldProperties = true;
            parsed.worldProperties.waterSourceDepth =
                record["world_properties"].value("water_source_depth", 1000);
            // Умолчания — как в WorldPropertiesComponent: у мира,
            // сохранённого до появления этих полей, будут они, а не нули
            // (нулевое испарение при работающих источниках залило бы такой
            // мир целиком).
            parsed.worldProperties.waterEvaporationRate =
                record["world_properties"].value("water_evaporation_rate", 20);
            parsed.worldProperties.rainIntervalTicks =
                record["world_properties"].value("rain_interval_ticks", 400);
            parsed.worldProperties.rainAmount = record["world_properties"].value("rain_amount", 50);
            parsed.worldProperties.soilErosionRate = record["world_properties"].value("soil_erosion_rate", 50);
            parsed.worldProperties.toggles.mineralsSpread =
                record["world_properties"].value("minerals_spread_enabled", true);
            parsed.worldProperties.toggles.erosionDeposition =
                record["world_properties"].value("erosion_deposition_enabled", true);
            parsed.worldProperties.plantMutationRate = record["world_properties"].value("plant_mutation_rate", 60);
            parsed.worldProperties.humusDecayPeriod =
                record["world_properties"].value("humus_decay_period", 50);
            parsed.worldProperties.plantRandomSeed =
                record["world_properties"].value("plant_random_seed", 0u);
            parsed.worldProperties.animalMutationRate =
                record["world_properties"].value("animal_mutation_rate", 0.06f);
            parsed.worldProperties.animalRandomSeed =
                record["world_properties"].value("animal_random_seed", 0u);
        }
        if (record.contains("plant_species") && record["plant_species"].is_array()) {
            parsed.hasPlantSpecies = true;
            for (const auto& archetype : record["plant_species"]) {
                if (archetype.is_object()) {
                    parsed.plantSpecies.push_back(genomeFromJson<PlantGenomeComponent>(archetype, kGrassTraits));
                }
            }
        }
        if (record.contains("animal_species") && record["animal_species"].is_object()) {
            parsed.hasAnimalSpecies = true;
            const auto& lists = record["animal_species"];
            if (lists.contains("herbivores") && lists["herbivores"].is_array()) {
                for (const auto& archetype : lists["herbivores"]) {
                    if (archetype.is_object()) {
                        parsed.animalSpecies.herbivores.push_back(
                            genomeFromJson<AnimalGenomeComponent>(archetype, kHerbivoreTraits));
                    }
                }
            }
            if (lists.contains("predators") && lists["predators"].is_array()) {
                for (const auto& archetype : lists["predators"]) {
                    if (archetype.is_object()) {
                        parsed.animalSpecies.predators.push_back(
                            genomeFromJson<AnimalGenomeComponent>(archetype, kPredatorTraits));
                    }
                }
            }
        } else if (record.contains("herbivore_species") && record["herbivore_species"].is_array()) {
            // Файл, сохранённый до появления хищников: там виды травоядных
            // лежали отдельным ключом и списком. Читаем их как список
            // травоядных — мир от этого не меняется, а хищников в нём
            // просто нет.
            parsed.hasAnimalSpecies = true;
            for (const auto& archetype : record["herbivore_species"]) {
                if (archetype.is_object()) {
                    parsed.animalSpecies.herbivores.push_back(
                        genomeFromJson<AnimalGenomeComponent>(archetype, kHerbivoreTraits));
                }
            }
        }
        if (record.contains("position")) {
            parsed.hasPosition = true;
            parsed.position.x = record["position"].value("x", 0);
            parsed.position.y = record["position"].value("y", 0);
            if (parsed.position.x < 0 || parsed.position.x >= width || parsed.position.y < 0 ||
                parsed.position.y >= height) {
                outError = "entity position (" + std::to_string(parsed.position.x) + "," +
                            std::to_string(parsed.position.y) + ") is outside the saved area";
                return false;
            }
        }
        if (record.contains("soil")) {
            parsed.hasSoil = true;
            parsed.soil.moisture = record["soil"].value("moisture", 0.0f);
            parsed.soil.rockiness = record["soil"].value("rockiness", 0.0f);
            parsed.soil.minerals = record["soil"].value("minerals", 0);
        }
        parsed.height = record.value("height", 0);
        if (record.contains("water")) {
            parsed.hasWater = true;
            // Старые файлы несут ещё и "flow_speed" — его молча
            // игнорируем: признака "река" у воды больше нет (см.
            // WaterComponent), а нечитаемым из-за лишнего ключа файл
            // становиться не должен.
            parsed.water.depth = record["water"].value("depth", 0);
            parsed.water.flowRemainder = record["water"].value("flow_remainder", 0);
        }
        parsed.impassable = record.value("impassable", false);
        parsed.waterSource = record.value("water_source", false);
        if (record.contains("humus")) {
            parsed.hasHumus = true;
            parsed.humus.minerals = record["humus"].value("minerals", 0);
        }
        if (record.contains("carcass")) {
            parsed.hasCarcass = true;
            parsed.carcass.meat = record["carcass"].value("meat", 0.0f);
            parsed.carcass.protein = record["carcass"].value("protein", 0);
        }
        if (record.contains("plant")) {
            parsed.hasPlant = true;
            parsed.plant.age = record["plant"].value("age", 0.0f);
            parsed.plant.growth = record["plant"].value("growth", 0.0f);
            parsed.plant.minerals = record["plant"].value("minerals", 0);
            parsed.plant.stress = record["plant"].value("stress", 0.0f);
            // Геном обязателен: растение без него не смогло бы ни расти,
            // ни размножаться, а подставлять "средний геном" значило бы
            // втихую менять состояние мира при загрузке.
            if (!record.contains("genome")) {
                outError = "plant entity has no genome";
                return false;
            }
            parsed.genome = genomeFromJson<PlantGenomeComponent>(record["genome"], kGrassTraits);
        }
        if (record.contains("seed")) {
            parsed.hasSeed = true;
            parsed.seed.age = record["seed"].value("age", 0.0f);
            parsed.seed.minerals = record["seed"].value("minerals", 0);
            // Геном обязателен по той же причине, что и у растения: из
            // семени без генома нечему было бы прорасти, а "средний геном"
            // втихую изменил бы состояние мира при загрузке.
            if (!record.contains("genome")) {
                outError = "seed entity has no genome";
                return false;
            }
            parsed.genome = genomeFromJson<PlantGenomeComponent>(record["genome"], kGrassTraits);
        }
        // Диета — тег, как impassable. Отдельная сложность только одна: в
        // файлах, сохранённых до появления хищников, под ключом "herbivore"
        // лежал ОБЪЕКТ с телом животного, а не признак диеты. Различаем их
        // по типу значения — так старый мир открывается как открывался, а
        // новый пишется без оглядки на прошлое.
        const bool legacyBody = record.contains("herbivore") && record["herbivore"].is_object();
        parsed.predator = record.value("predator", false);

        const char* bodyKey = record.contains("animal") ? "animal" : (legacyBody ? "herbivore" : nullptr);
        if (bodyKey != nullptr) {
            parsed.hasAnimal = true;
            const auto& animal = record[bodyKey];
            parsed.animal.age = animal.value("age", 0.0f);
            parsed.animal.growth = animal.value("growth", 0.0f);
            parsed.animal.sex = sexFromName(animal.value("sex", std::string("female")));
            parsed.animal.energy = animal.value("energy", 0.0f);
            parsed.animal.water = animal.value("water", 0.0f);
            parsed.animal.protein = animal.value("protein", 0);
            parsed.animal.dung = animal.value("dung", 0);
            // У животного из старого файла целости тела нет — ран тогда
            // никто не наносил, значит оно цело.
            parsed.animal.health = animal.value("health", 1.0f);
            parsed.animal.stepProgress = animal.value("step_progress", 0.0f);
            // Геном обязателен по той же причине, что и у растения:
            // подставить "средний геном" значило бы втихую изменить
            // состояние мира при загрузке.
            const char* genomeKey = record.contains("animal_genome")
                                         ? "animal_genome"
                                         : (record.contains("herbivore_genome") ? "herbivore_genome" : nullptr);
            if (genomeKey == nullptr) {
                outError = "animal entity has no genome";
                return false;
            }
            parsed.animalGenome = parsed.predator
                                       ? genomeFromJson<AnimalGenomeComponent>(record[genomeKey], kPredatorTraits)
                                       : genomeFromJson<AnimalGenomeComponent>(record[genomeKey], kHerbivoreTraits);

            if (record.contains("desire")) {
                const auto& desire = record["desire"];
                // Голод, жажда и страх из старых файлов молча
                // игнорируются: они пересчитываются из тела на первом же
                // тике, и хранить их между запусками мира незачем.
                parsed.desire.mating = desire.value("mating", 0.0f);
                parsed.desire.current = desireFromName(desire.value("current", std::string("idle")));
            }
            // Постоянный идентификатор — единственное, что животное не может
            // восстановить само: он и есть его ключ случайности. Если файл
            // его почему-то не несёт, выдаём новый из позиции и состояния —
            // животное останется собой во всём, кроме прошлых бросков
            // жребия, а мир и так недетерминирован
            // (02_CorePrinciples.md, п.12a).
            parsed.identity = record.value("identity", static_cast<std::uint64_t>(0));
            if (parsed.identity == 0) {
                parsed.identity = mixSeed(static_cast<std::uint64_t>(parsed.position.x * 73856093 +
                                                                     parsed.position.y * 19349663),
                                           static_cast<std::uint64_t>(outEntities.size()) + 1ull);
            }
        }

        // Позиция — единственное, чем Entity привязан к тайлу; без неё
        // Area не знает, куда его положить, а непроходимость становится
        // бессмысленной (04_WorldModel.md, п.4).
        if (!parsed.hasPosition && (parsed.hasSoil || parsed.hasWater || parsed.impassable || parsed.waterSource ||
                                     parsed.hasPlant || parsed.hasAnimal || parsed.hasHumus ||
                                     parsed.hasCarcass)) {
            outError = "entity has world components but no position";
            return false;
        }

        outEntities.push_back(parsed);
    }

    return true;
}

std::optional<std::string> readCreatedAt(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    const auto json = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.contains("info")) {
        return std::nullopt;
    }
    const auto created = json["info"].value("created_at", std::string{});
    if (created.empty()) {
        return std::nullopt;
    }
    return created;
}

} // namespace

std::filesystem::path resolveSaveDirectory(const std::string& configured) {
    const std::filesystem::path path(configured.empty() ? std::string("saves") : configured);
    if (path.is_absolute()) {
        return path;
    }
    return getExecutableDirectory() / path;
}

bool isValidWorldName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    if (name.front() == '.' || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    if (name.find("..") != std::string::npos) {
        return false;
    }
    for (const char c : name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                             c == '_' || c == '.' || c == ' ';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string makeUniqueWorldName(const std::filesystem::path& directory) {
    const std::string base = "world-" + formatUtcNow("%Y%m%d-%H%M%S");
    std::string candidate = base;
    std::error_code ec;
    for (int suffix = 2; std::filesystem::exists(savePath(directory, candidate), ec); ++suffix) {
        candidate = base + "-" + std::to_string(suffix);
    }
    return candidate;
}

std::vector<WorldSaveInfo> listWorldSaves(const std::filesystem::path& directory) {
    std::vector<WorldSaveInfo> worlds;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return worlds;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != kExtension) {
            continue;
        }

        std::ifstream file(entry.path());
        if (!file.is_open()) {
            std::cerr << "WorldSave: could not open '" << entry.path().string() << "'.\n";
            continue;
        }

        // Файл читается целиком: заголовок лежит внутри того же
        // JSON-объекта, что и состояние мира. Отдельный индекс миров был
        // бы дешевле, но он неизбежно разъезжается с реальными файлами
        // (удалили файл вручную — индекс врёт), а список запрашивается
        // редко: при подключении клиента и при открытии экрана выбора
        // мира.
        const auto json = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        if (json.is_discarded() || json.value("format", std::string{}) != kFormatTag) {
            std::cerr << "WorldSave: '" << entry.path().string() << "' is not a world save, skipping.\n";
            continue;
        }

        WorldSaveInfo info;
        try {
            info = json.at("info").get<WorldSaveInfo>();
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "WorldSave: '" << entry.path().string() << "' has a broken header (" << e.what()
                       << "), skipping.\n";
            continue;
        }

        // Источник истины для имени — имя файла, а не поле в нём: файл
        // могли переименовать, и именно по имени файла мир потом
        // загружается.
        info.name = entry.path().stem().string();
        worlds.push_back(std::move(info));
    }

    // Свежие сверху — это порядок, в котором миры показываются в меню.
    std::sort(worlds.begin(), worlds.end(), [](const WorldSaveInfo& a, const WorldSaveInfo& b) {
        if (a.saved_at != b.saved_at) {
            return a.saved_at > b.saved_at;
        }
        return a.name < b.name;
    });

    return worlds;
}

bool saveWorld(const World& world, const RegenerationRequest& generation, const PopulationHistory& history,
               const std::string& name, const std::filesystem::path& directory, WorldSaveInfo& outInfo,
               std::string& outError) {
    if (!isValidWorldName(name)) {
        outError = "invalid world name '" + name + "'";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        outError = "could not create save directory '" + directory.string() + "' (" + ec.message() + ")";
        return false;
    }

    const auto path = savePath(directory, name);
    const auto now = utcTimestamp();

    WorldSaveInfo info;
    info.name = name;
    info.tick = world.registry().get<const TimeComponent>(world.worldEntity()).tick;
    info.area_width = world.area().width();
    info.area_height = world.area().height();
    info.seed = generation.seed;
    // Мир создаётся один раз, а сохраняется многократно — момент
    // создания переносится из предыдущей версии файла, если она есть.
    info.created_at = readCreatedAt(path).value_or(now);
    info.saved_at = now;

    nlohmann::json json;
    json["format"] = kFormatTag;
    json["version"] = kWorldSaveFormatVersion;
    json["info"] = info;
    json["generation"] = generation;
    // Летопись численности — рядом с миром, а не в отдельном файле: она
    // описывает жизнь именно этого мира, и разъехаться с ним (удалили мир,
    // осталась летопись) не должна.
    json["history"] = history.toJson();
    json["entities"] = buildEntitiesJson(world);

    // Пишем во временный файл и переименовываем поверх: прерванное на
    // середине сохранение (упал процесс, кончилось место) не должно
    // оставить вместо рабочего мира обрезанный файл.
    const auto tempPath = std::filesystem::path(path).replace_extension(".tmp");
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            outError = "could not write '" + tempPath.string() + "'";
            return false;
        }
        file << json.dump();
        file.flush();
        if (!file) {
            outError = "failed while writing '" + tempPath.string() + "'";
            return false;
        }
    }

    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        outError = "could not replace '" + path.string() + "'";
        return false;
    }

    outInfo = info;
    return true;
}

bool loadWorld(World& world, const std::string& name, const std::filesystem::path& directory,
               RegenerationRequest& outGeneration, PopulationHistory& outHistory, WorldSaveInfo& outInfo,
               std::string& outError) {
    if (!isValidWorldName(name)) {
        outError = "invalid world name '" + name + "'";
        return false;
    }

    const auto path = savePath(directory, name);
    std::ifstream file(path);
    if (!file.is_open()) {
        outError = "no saved world '" + name + "' in '" + directory.string() + "'";
        return false;
    }

    const auto json = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        outError = "'" + path.string() + "' is not valid JSON";
        return false;
    }
    if (json.value("format", std::string{}) != kFormatTag) {
        outError = "'" + path.string() + "' is not a world save";
        return false;
    }
    if (json.value("version", 0) != kWorldSaveFormatVersion) {
        outError = "'" + path.string() + "' has unsupported format version " +
                    std::to_string(json.value("version", 0));
        return false;
    }

    WorldSaveInfo info;
    RegenerationRequest generation;
    try {
        info = json.at("info").get<WorldSaveInfo>();
        generation = json.at("generation").get<RegenerationRequest>();
    } catch (const nlohmann::json::exception& e) {
        outError = std::string("'") + path.string() + "' is broken (" + e.what() + ")";
        return false;
    }
    info.name = name;

    if (info.area_width <= 0 || info.area_height <= 0) {
        outError = "'" + path.string() + "' has an empty area";
        return false;
    }

    std::vector<ParsedEntity> entities;
    if (!json.contains("entities") ||
        !parseEntities(json["entities"], info.area_width, info.area_height, entities, outError)) {
        if (outError.empty()) {
            outError = "'" + path.string() + "' has no entities";
        }
        return false;
    }

    // Файл разобран целиком и оказался непротиворечивым — только теперь
    // трогаем сам мир.
    world.reset(info.area_width, info.area_height);

    std::uint64_t tick = info.tick;
    for (const auto& parsed : entities) {
        if (parsed.hasTime || parsed.hasWorldProperties || parsed.hasPlantSpecies || parsed.hasAnimalSpecies) {
            // World Entity уже существует (создан в reset, со значениями
            // по умолчанию для свойств мира) — у мира он один, поэтому
            // эта запись просто уточняет его данные, а не создаёт второй
            // "мировой" Entity.
            if (parsed.hasTime) {
                tick = parsed.tick;
            }
            if (parsed.hasWorldProperties) {
                world.registry().get<WorldPropertiesComponent>(world.worldEntity()) = parsed.worldProperties;
            }
            if (parsed.hasPlantSpecies) {
                world.registry().get<PlantSpeciesComponent>(world.worldEntity()).archetypes = parsed.plantSpecies;
            }
            if (parsed.hasAnimalSpecies) {
                world.registry().get<AnimalSpeciesComponent>(world.worldEntity()) = parsed.animalSpecies;
            }
            continue;
        }

        const auto entity = world.registry().create();
        if (parsed.hasSoil) {
            world.registry().emplace<SoilComponent>(entity, parsed.soil);
            // Высота всегда идёт в паре с почвой на террейн-Entity, как и
            // при генерации; в старых сохранениях без поля "height" —
            // 0.0f (плоский рельеф), см. WorldSave.hpp.
            world.registry().emplace<HeightComponent>(entity, HeightComponent{parsed.height});
        }
        if (parsed.hasWater) {
            world.registry().emplace<WaterComponent>(entity, parsed.water);
        }
        if (parsed.hasHumus) {
            world.registry().emplace<HumusComponent>(entity, parsed.humus);
        }
        if (parsed.hasPlant) {
            world.registry().emplace<PlantComponent>(entity, parsed.plant);
            world.registry().emplace<PlantGenomeComponent>(entity, parsed.genome);
        }
        if (parsed.hasSeed) {
            world.registry().emplace<SeedComponent>(entity, parsed.seed);
            world.registry().emplace<PlantGenomeComponent>(entity, parsed.genome);
        }
        if (parsed.hasAnimal) {
            world.registry().emplace<AnimalComponent>(entity, parsed.animal);
            world.registry().emplace<AnimalGenomeComponent>(entity, parsed.animalGenome);
            world.registry().emplace<DesireComponent>(entity, parsed.desire);
            world.registry().emplace<IdentityComponent>(entity, IdentityComponent{parsed.identity});
            // Память ног (MovementComponent) в файле не лежит и лежать не
            // должна: шесть последних шагов — это походка, а не мир (см. сам
            // компонент). Загруженное животное начинает помнить заново, и
            // единственное, что от этого меняется, — первые несколько его
            // шагов после открытия мира.
            world.registry().emplace<MovementComponent>(entity);
            // Хромота (InjuryComponent) в файле не лежит по той же
            // причине: срок увечья — не мир, а состояние тела на несколько
            // сотен тиков. Загруженный зверь просто здоров.
            world.registry().emplace<InjuryComponent>(entity);
            // Диета: без тега животное не знало бы, что для него еда.
            // Хищник помечен явно, всё остальное живое — травоядное (в том
            // числе животные из файлов, сохранённых до появления хищников).
            if (parsed.predator) {
                world.registry().emplace<PredatorComponent>(entity);
            } else {
                world.registry().emplace<HerbivoreComponent>(entity);
            }
        }
        if (parsed.hasCarcass) {
            world.registry().emplace<CarcassComponent>(entity, parsed.carcass);
        }
        if (parsed.impassable) {
            world.registry().emplace<ImpassableComponent>(entity);
        }
        if (parsed.waterSource) {
            world.registry().emplace<WaterSourceComponent>(entity);
        }
        if (parsed.hasPosition) {
            // Индекс размещения Area в файле не хранится — он
            // восстанавливается из позиций, как при генерации. Отсюда же
            // Entity получает и сам PositionComponent: позиция и место в
            // Area выставляются только вместе (World::place), поэтому
            // размещение идёт последним — ImpassableComponent к этому
            // моменту уже на месте.
            world.place(entity, parsed.position.x, parsed.position.y);
        }
    }

    world.registry().get<TimeComponent>(world.worldEntity()).tick = tick;
    info.tick = tick;

    // Летопись — последней, вместе с самим миром: у файла без неё (старое
    // сохранение) она просто окажется пустой и начнётся заново с этого
    // тика. Битые точки внутри неё мир загрузить не мешают — это
    // наблюдение за миром, а не сам мир (см. PopulationHistory::fromJson).
    outHistory.fromJson(json.contains("history") ? json["history"] : nlohmann::json::object());

    outGeneration = generation;
    outInfo = info;
    return true;
}

bool deleteWorld(const std::string& name, const std::filesystem::path& directory, std::string& outError) {
    if (!isValidWorldName(name)) {
        outError = "invalid world name '" + name + "'";
        return false;
    }

    const auto path = savePath(directory, name);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        outError = "no saved world '" + name + "' in '" + directory.string() + "'";
        return false;
    }

    if (!std::filesystem::remove(path, ec) || ec) {
        outError = "could not delete '" + path.string() + "'" + (ec ? " (" + ec.message() + ")" : "");
        return false;
    }

    return true;
}

} // namespace goblins

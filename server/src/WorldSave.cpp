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

#include "core/components/HeightComponent.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
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

// Геном пишется и читается обходом таблицы черт (core::kGrassTraits), а не
// перечислением полей: имена в файле — это имена черт, поэтому новая
// черта попадает в сохранение сама. Отсутствующая в старом файле черта
// берёт значение по умолчанию — та же логика, что и у "height"/"minerals"
// (см. заголовок WorldSave.hpp): формат от этого не ломается и версию
// поднимать не нужно.
nlohmann::json genomeToJson(const PlantGenomeComponent& genome) {
    nlohmann::json record;
    record["species"] = genome.species;
    for (const auto& trait : kGrassTraits) {
        record[trait.name] = genome.*trait.gene;
    }
    return record;
}

PlantGenomeComponent genomeFromJson(const nlohmann::json& record) {
    PlantGenomeComponent genome;
    genome.species = record.value("species", 0);
    for (const auto& trait : kGrassTraits) {
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
    // (parsed.hasSoil), а для старых сохранений без поля "height" уже есть
    // безопасное значение по умолчанию — 0.0f.
    float height = 0.0f;
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
    // 06_GameLoop.md, п.1a): выбраны при генерации и не меняются.
    bool hasPlantSpecies = false;
    std::vector<PlantGenomeComponent> plantSpecies;

    // Живое растение — Entity с состоянием и геномом (оба обязательно
    // вместе: растение без генома не смогло бы ни расти, ни дать потомка).
    bool hasPlant = false;
    PlantComponent plant{};
    PlantGenomeComponent genome{};

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
            record["world_properties"] = {{"mineral_moisture_threshold", worldProperties->mineralMoistureThreshold},
                                          {"water_evaporation_rate", worldProperties->waterEvaporationRate},
                                          {"water_source_strength", worldProperties->waterSourceStrength},
                                          {"water_flow_rate", worldProperties->waterFlowRate},
                                          {"water_slope_boost", worldProperties->waterSlopeBoost},
                                          {"soil_erosion_rate", worldProperties->soilErosionRate},
                                          {"max_erosion_depth", worldProperties->maxErosionDepth},
                                          {"edge_drain_rate", worldProperties->edgeDrainRate},
                                          {"plant_mutation_rate", worldProperties->plantMutationRate},
                                          {"humus_decay_rate", worldProperties->humusDecayRate},
                                          {"plant_random_seed", worldProperties->plantRandomSeed}};
        }
        if (const auto* plantSpecies = registry.try_get<PlantSpeciesComponent>(entity)) {
            auto archetypes = nlohmann::json::array();
            for (const auto& archetype : plantSpecies->archetypes) {
                archetypes.push_back(genomeToJson(archetype));
            }
            record["plant_species"] = std::move(archetypes);
        }
        if (const auto* position = registry.try_get<PositionComponent>(entity)) {
            record["position"] = {{"x", position->x}, {"y", position->y}};
        }
        if (const auto* soil = registry.try_get<SoilComponent>(entity)) {
            record["soil"] = {{"moisture", soil->moisture},
                              {"rockiness", soil->rockiness},
                              {"compaction", soil->compaction},
                              {"minerals", soil->minerals}};
        }
        if (const auto* heightComponent = registry.try_get<HeightComponent>(entity)) {
            record["height"] = heightComponent->height;
        }
        if (const auto* water = registry.try_get<WaterComponent>(entity)) {
            record["water"] = {{"depth", water->depth}, {"flow_speed", water->flowSpeed}};
        }
        if (const auto* humus = registry.try_get<HumusComponent>(entity)) {
            record["humus"] = {{"minerals", humus->minerals}, {"pending", humus->pending}};
        }
        if (const auto* plant = registry.try_get<PlantComponent>(entity)) {
            record["plant"] = {{"age", plant->age},
                               {"growth", plant->growth},
                               {"moisture", plant->moisture},
                               {"minerals", plant->minerals},
                               {"mineral_pending", plant->mineralPending},
                               {"stress", plant->stress}};
        }
        if (const auto* genome = registry.try_get<PlantGenomeComponent>(entity)) {
            record["genome"] = genomeToJson(*genome);
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
            parsed.worldProperties.mineralMoistureThreshold =
                record["world_properties"].value("mineral_moisture_threshold", 0.5f);
            parsed.worldProperties.waterEvaporationRate =
                record["world_properties"].value("water_evaporation_rate", 0.00004f);
            parsed.worldProperties.waterSourceStrength =
                record["world_properties"].value("water_source_strength", 0.05f);
            parsed.worldProperties.waterFlowRate =
                record["world_properties"].value("water_flow_rate", 0.3f);
            parsed.worldProperties.waterSlopeBoost =
                record["world_properties"].value("water_slope_boost", 5.0f);
            parsed.worldProperties.soilErosionRate =
                record["world_properties"].value("soil_erosion_rate", 0.05f);
            parsed.worldProperties.maxErosionDepth =
                record["world_properties"].value("max_erosion_depth", 0.5f);
            parsed.worldProperties.edgeDrainRate =
                record["world_properties"].value("edge_drain_rate", 0.01f);
            parsed.worldProperties.plantMutationRate =
                record["world_properties"].value("plant_mutation_rate", 0.06f);
            parsed.worldProperties.humusDecayRate =
                record["world_properties"].value("humus_decay_rate", 0.02f);
            parsed.worldProperties.plantRandomSeed =
                record["world_properties"].value("plant_random_seed", 0u);
        }
        if (record.contains("plant_species") && record["plant_species"].is_array()) {
            parsed.hasPlantSpecies = true;
            for (const auto& archetype : record["plant_species"]) {
                if (archetype.is_object()) {
                    parsed.plantSpecies.push_back(genomeFromJson(archetype));
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
            parsed.soil.compaction = record["soil"].value("compaction", 0.0f);
            parsed.soil.minerals = record["soil"].value("minerals", 0);
        }
        parsed.height = record.value("height", 0.0f);
        if (record.contains("water")) {
            parsed.hasWater = true;
            parsed.water.depth = record["water"].value("depth", 0.0f);
            parsed.water.flowSpeed = record["water"].value("flow_speed", 0.0f);
        }
        parsed.impassable = record.value("impassable", false);
        parsed.waterSource = record.value("water_source", false);
        if (record.contains("humus")) {
            parsed.hasHumus = true;
            parsed.humus.minerals = record["humus"].value("minerals", 0);
            parsed.humus.pending = record["humus"].value("pending", 0.0f);
        }
        if (record.contains("plant")) {
            parsed.hasPlant = true;
            parsed.plant.age = record["plant"].value("age", 0.0f);
            parsed.plant.growth = record["plant"].value("growth", 0.0f);
            parsed.plant.moisture = record["plant"].value("moisture", 0.0f);
            parsed.plant.minerals = record["plant"].value("minerals", 0);
            parsed.plant.mineralPending = record["plant"].value("mineral_pending", 0.0f);
            parsed.plant.stress = record["plant"].value("stress", 0.0f);
            // Геном обязателен: растение без него не смогло бы ни расти,
            // ни размножаться, а подставлять "средний геном" значило бы
            // втихую менять состояние мира при загрузке.
            if (!record.contains("genome")) {
                outError = "plant entity has no genome";
                return false;
            }
            parsed.genome = genomeFromJson(record["genome"]);
        }

        // Позиция — единственное, чем Entity привязан к тайлу; без неё
        // Area не знает, куда его положить, а непроходимость становится
        // бессмысленной (04_WorldModel.md, п.4).
        if (!parsed.hasPosition && (parsed.hasSoil || parsed.hasWater || parsed.impassable || parsed.waterSource ||
                                     parsed.hasPlant || parsed.hasHumus)) {
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

bool saveWorld(const World& world, const RegenerationRequest& generation, const std::string& name,
               const std::filesystem::path& directory, WorldSaveInfo& outInfo, std::string& outError) {
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
    info.terrain_seed = generation.terrain_seed;
    info.boulder_seed = generation.boulder_seed;
    // Мир создаётся один раз, а сохраняется многократно — момент
    // создания переносится из предыдущей версии файла, если она есть.
    info.created_at = readCreatedAt(path).value_or(now);
    info.saved_at = now;

    nlohmann::json json;
    json["format"] = kFormatTag;
    json["version"] = kWorldSaveFormatVersion;
    json["info"] = info;
    json["generation"] = generation;
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
               RegenerationRequest& outGeneration, WorldSaveInfo& outInfo, std::string& outError) {
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
        if (parsed.hasTime || parsed.hasWorldProperties || parsed.hasPlantSpecies) {
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
            continue;
        }

        const auto entity = world.registry().create();
        if (parsed.hasPosition) {
            world.registry().emplace<PositionComponent>(entity, parsed.position);
        }
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
        if (parsed.impassable) {
            world.registry().emplace<ImpassableComponent>(entity);
        }
        if (parsed.waterSource) {
            world.registry().emplace<WaterSourceComponent>(entity);
        }
        if (parsed.hasPosition) {
            // Индекс размещения Area в файле не хранится — он
            // восстанавливается из позиций, как при генерации.
            world.area().place(entity, parsed.position.x, parsed.position.y, parsed.impassable);
        }
    }

    world.registry().get<TimeComponent>(world.worldEntity()).tick = tick;
    info.tick = tick;

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

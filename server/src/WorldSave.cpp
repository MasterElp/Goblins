#include "server/WorldSave.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
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
#include "core/PlantKind.hpp"
#include "core/components/BerryComponent.hpp"
#include "core/components/CarriedComponent.hpp"
#include "core/components/BushComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/GoblinDesireComponent.hpp"
#include "core/components/GoblinTribesComponent.hpp"
#include "core/components/KnowledgeComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SeedComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/StoreComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/GoblinGenetics.hpp"
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
    // Виды деревьев — там же и по тем же правилам, отдельным списком: своя
    // нумерация, своя таблица черт (PlantSpeciesComponent).
    bool hasTreeSpecies = false;
    std::vector<PlantGenomeComponent> treeSpecies;

    // Виды кустов — третий список по той же причине, что и второй: своя
    // нумерация и своя таблица черт (kBushTraits).
    bool hasBushSpecies = false;
    std::vector<PlantGenomeComponent> bushSpecies;
    bool hasAnimalSpecies = false;
    AnimalSpeciesComponent animalSpecies{};
    // Племена гоблинов — там же и по тем же правилам, своим списком: таблица
    // черт у них своя (kGoblinTraits), и племя — это вид внутри неё.
    bool hasGoblinTribes = false;
    GoblinTribesComponent goblinTribes{};

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

    // Дерево — тег, как impassable: растение с ним живёт по древесным
    // законам (core/Trees.hpp) и читает свой геном по древесной таблице.
    // Лежит и на семени дерева, поэтому проверяется отдельно от hasPlant.
    bool tree = false;

    // Метка куста — рядом с древесной и по той же причине: она решает, по
    // какой таблице читается геном и чем растение станет при загрузке.
    bool bush = false;
    // Ягоды на кусте. Отдельным компонентом (BerryComponent), и в файле
    // тоже отдельно: без них загруженный ягодник встретил бы гоблинов
    // пустым, и весь дневной круг племени начался бы заново.
    bool hasBerries = false;
    BerryComponent berries{};

    // Куча принесённой еды на тайле (core/Store.hpp) — состояние земли, как
    // перегной и падаль.
    bool hasStore = false;
    StoreComponent store{};

    // Что существо несёт в руках. Ноша — общее свойство живого, поэтому
    // разбирается рядом с телом, а не внутри гоблинской ветки.
    bool hasCarried = false;
    CarriedComponent carried{};

    // Живое животное — Entity с телом, геномом, желаниями, постоянным
    // идентификатором и тегом диеты. Всё это обязательно вместе: без
    // генома животное не смогло бы ни расти, ни принести потомство, без
    // идентификатора у него не было бы ключа случайности (см.
    // IdentityComponent), желания — то, чем оно занято прямо сейчас, а без
    // диеты оно не знало бы, что для него еда.
    bool hasAnimal = false;
    bool predator = false;
    // Гоблин: то же тело, но своя таблица черт и своё желание.
    bool goblin = false;
    GoblinComponent goblinState{};
    KnowledgeComponent goblinMind{};
    AnimalComponent animal{};
    AnimalGenomeComponent animalGenome{};
    DesireComponent desire{};
    GoblinDesireComponent goblinDesire{};
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
            for (const auto& archetype : plantSpecies->grasses) {
                archetypes.push_back(genomeToJson(archetype, kGrassTraits));
            }
            record["plant_species"] = std::move(archetypes);
            auto treeArchetypes = nlohmann::json::array();
            for (const auto& archetype : plantSpecies->trees) {
                treeArchetypes.push_back(genomeToJson(archetype, kTreeTraits));
            }
            record["tree_species"] = std::move(treeArchetypes);
            auto bushArchetypes = nlohmann::json::array();
            for (const auto& archetype : plantSpecies->bushes) {
                bushArchetypes.push_back(genomeToJson(archetype, kBushTraits));
            }
            record["bush_species"] = std::move(bushArchetypes);
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
        if (const auto* tribes = registry.try_get<GoblinTribesComponent>(entity)) {
            // Список один, в отличие от двух звериных: таблица черт у гоблинов
            // одна. Без этой записи загруженный мир остаётся с гоблинами, но
            // без племён — мутациям становится не вокруг чего гулять
            // (kSpeciesBand), и за поколения племена слились бы в одно.
            auto archetypes = nlohmann::json::array();
            for (const auto& archetype : tribes->tribes) {
                archetypes.push_back(genomeToJson(archetype, kGoblinTraits));
            }
            record["goblin_tribes"] = std::move(archetypes);
        }
        if (const auto* position = registry.try_get<PositionComponent>(entity)) {
            record["position"] = {{"x", position->x}, {"y", position->y}};
        }
        if (const auto* soil = registry.try_get<SoilComponent>(entity)) {
            record["soil"] = {{"moisture", soil->moisture},
                              {"rockiness", soil->rockiness},
                              {"trampled", soil->trampled},
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
            record["seed"] = {{"age", seed->age}};
        }
        // Ягоды и ушедшие в них крупицы — состояние куста, а не его вида.
        // Крупицы пишутся вместе с ягодами: они уже вынуты из земли, и
        // потеряться при загрузке им нельзя — вещество в этом мире не
        // появляется и не пропадает.
        // Куча — принесённое, а не выросшее: потеряться ей нельзя тем более,
        // что восстановиться сама она не может. Крупицы пишутся вместе с
        // едой: они уже вынуты из земли и вернутся в неё либо через
        // съевшего, либо гниением (core/Store.hpp).
        if (const auto* store = registry.try_get<StoreComponent>(entity)) {
            record["store"] = {{"food", store->food}, {"minerals", store->minerals}};
        }
        if (const auto* berries = registry.try_get<BerryComponent>(entity)) {
            record["berries"] = {{"berries", berries->berries}, {"minerals", berries->minerals}};
        }
        if (const auto* genome = registry.try_get<PlantGenomeComponent>(entity)) {
            // Таблица черт выбирается по метке дерева: имена черт у травы и
            // у дерева сейчас совпадают, но совпадают они случайно, а не по
            // договору — древесная черта, которой нет у травы, иначе молча
            // не доехала бы до файла.
            switch (plantKindOf(registry, entity)) {
                case PlantKind::Tree: record["genome"] = genomeToJson(*genome, kTreeTraits); break;
                case PlantKind::Bush: record["genome"] = genomeToJson(*genome, kBushTraits); break;
                case PlantKind::Grass: record["genome"] = genomeToJson(*genome, kGrassTraits); break;
            }
        }
        // Гоблин носит то же тело, но записан как животное он быть не
        // может: при чтении ему достался бы тег диеты, и в мир вернулся бы
        // травоядный зверь с гоблинским геномом. Поэтому он помечается, и по
        // этой метке читаются его геном (своя таблица черт) и его желание
        // (своё перечисление).
        //
        // Тело при этом пишется тем же ключом "animal", что и у зверя, и это
        // не небрежность: тело у них одно и то же (core/Body.hpp), а два
        // ключа для одного компонента разошлись бы при первой же правке.
        const auto* goblinState = registry.try_get<GoblinComponent>(entity);
        const bool goblin = goblinState != nullptr;
        if (goblin) {
            // Не просто метка: усталость — накопленное состояние, и в теле
            // её не прочитать заново (см. GoblinComponent). Потерять её
            // значило бы вернуть из файла поселенца, который только что
            // выспался, где бы он ни был застигнут сохранением.
            record["goblin"] = {{"fatigue", goblinState->fatigue}};
        }
        // Ноша — рядом с телом, а не внутри метки гоблина: нести может всякий
        // живой (docs/10_Goblins.md, п.1a). Пустые руки не пишутся: их
        // незачем возить.
        if (const auto* carried = registry.try_get<CarriedComponent>(entity)) {
            if (carried->food > 0 || carried->minerals > 0) {
                record["carried"] = {{"food", carried->food}, {"minerals", carried->minerals}};
            }
        }
        if (const auto* mind = registry.try_get<KnowledgeComponent>(entity)) {
            // Память — состояние мира, а не походка: гоблин, забывший при
            // загрузке всё, начал бы набивать свои тропы заново, и открытый
            // мир оказался бы не тем, который сохраняли. Пустые места не
            // пишутся — их незачем возить.
            auto places = nlohmann::json::array();
            for (const auto& place : mind->places) {
                if (place.kind == PlaceKind::None || place.strength <= 0) {
                    continue;
                }
                places.push_back({{"x", place.x},
                                   {"y", place.y},
                                   {"kind", placeKindName(place.kind)},
                                   {"strength", place.strength}});
            }
            if (!places.empty()) {
                record["knows"] = std::move(places);
            }
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
            record["animal_genome"] =
                goblin ? genomeToJson(*genome, kGoblinTraits)
                       : (registry.all_of<PredatorComponent>(entity) ? genomeToJson(*genome, kPredatorTraits)
                                                                     : genomeToJson(*genome, kHerbivoreTraits));
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
        // Желание гоблина — своим ключом: перечисление у него другое, и имя
        // "food" в двух этих ключах означает разные законы.
        if (const auto* desire = registry.try_get<GoblinDesireComponent>(entity)) {
            record["goblin_desire"] = {{"mating", desire->mating},
                                        {"current", goblinDesireName(desire->current)}};
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
        // Дерево — такой же тег без данных. Стоит и на растении, и на
        // лежащем семени: и то, и другое живёт по древесным законам.
        if (registry.all_of<TreeComponent>(entity)) {
            record["tree"] = true;
        }
        // Куст — такой же тег без данных. Стоит и на растении, и на его
        // семени, если куст когда-нибудь начнёт сеять семенами.
        if (registry.all_of<BushComponent>(entity)) {
            record["bush"] = true;
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
// Геном растения читается по таблице СВОЕГО рода: имена черт у травы, куста
// и дерева сейчас совпадают, но совпадают случайно, а не по договору —
// черта, которой нет у соседнего рода, иначе молча не доехала бы из файла.
// Метки родов разбираются раньше генома именно поэтому.
PlantGenomeComponent plantGenomeFromRecord(const nlohmann::json& genome, bool tree, bool bush) {
    if (tree) {
        return genomeFromJson<PlantGenomeComponent>(genome, kTreeTraits);
    }
    if (bush) {
        return genomeFromJson<PlantGenomeComponent>(genome, kBushTraits);
    }
    return genomeFromJson<PlantGenomeComponent>(genome, kGrassTraits);
}

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
        if (record.contains("tree_species") && record["tree_species"].is_array()) {
            parsed.hasTreeSpecies = true;
            for (const auto& archetype : record["tree_species"]) {
                parsed.treeSpecies.push_back(genomeFromJson<PlantGenomeComponent>(archetype, kTreeTraits));
            }
        }
        if (record.contains("bush_species") && record["bush_species"].is_array()) {
            parsed.hasBushSpecies = true;
            for (const auto& archetype : record["bush_species"]) {
                parsed.bushSpecies.push_back(genomeFromJson(archetype, kBushTraits));
            }
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
        // Племена гоблинов — своим ключом и своей таблицей черт. Отдельной
        // проверкой, а не веткой в цепочке выше: они не заменяют собой виды
        // животных, они лежат рядом. У файлов, записанных до появления
        // гоблинов, ключа просто нет.
        if (record.contains("goblin_tribes") && record["goblin_tribes"].is_array()) {
            parsed.hasGoblinTribes = true;
            for (const auto& archetype : record["goblin_tribes"]) {
                if (archetype.is_object()) {
                    parsed.goblinTribes.tribes.push_back(
                        genomeFromJson<AnimalGenomeComponent>(archetype, kGoblinTraits));
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
            // Утоптанность: у файлов, записанных до появления троп, её нет,
            // и мир открывается нехоженым — это верно, троп в нём и не было.
            parsed.soil.trampled = record["soil"].value("trampled", 0);
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
        parsed.tree = record.value("tree", false);
        parsed.bush = record.value("bush", false);
        if (record.contains("store")) {
            parsed.hasStore = true;
            parsed.store.food = record["store"].value("food", 0);
            parsed.store.minerals = record["store"].value("minerals", 0);
        }
        if (record.contains("carried")) {
            parsed.hasCarried = true;
            parsed.carried.food = record["carried"].value("food", 0);
            parsed.carried.minerals = record["carried"].value("minerals", 0);
        }
        if (record.contains("berries")) {
            parsed.hasBerries = true;
            parsed.berries.berries = record["berries"].value("berries", 0);
            parsed.berries.minerals = record["berries"].value("minerals", 0);
        }
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
            parsed.genome = plantGenomeFromRecord(record["genome"], parsed.tree, parsed.bush);
        }
        if (record.contains("seed")) {
            parsed.hasSeed = true;
            parsed.seed.age = record["seed"].value("age", 0.0f);
            // Геном обязателен по той же причине, что и у растения: из
            // семени без генома нечему было бы прорасти, а "средний геном"
            // втихую изменил бы состояние мира при загрузке.
            if (!record.contains("genome")) {
                outError = "seed entity has no genome";
                return false;
            }
            parsed.genome = plantGenomeFromRecord(record["genome"], parsed.tree, parsed.bush);
        }
        // Диета — тег, как impassable. Отдельная сложность только одна: в
        // файлах, сохранённых до появления хищников, под ключом "herbivore"
        // лежал ОБЪЕКТ с телом животного, а не признак диеты. Различаем их
        // по типу значения — так старый мир открывается как открывался, а
        // новый пишется без оглядки на прошлое.
        const bool legacyBody = record.contains("herbivore") && record["herbivore"].is_object();
        parsed.predator = record.value("predator", false);
        // Метка гоблина была признаком true, а стала объектом с усталостью:
        // различаем по типу, чтобы миры, записанные до неё, открывались как
        // открывались — просто с невыспавшимися жителями.
        if (record.contains("goblin")) {
            const auto& mark = record["goblin"];
            parsed.goblin = mark.is_object() ? true : mark.get<bool>();
            if (mark.is_object()) {
                parsed.goblinState.fatigue = mark.value("fatigue", 0);
            }
        }
        if (record.contains("knows") && record["knows"].is_array()) {
            std::size_t slot = 0;
            for (const auto& place : record["knows"]) {
                if (slot >= parsed.goblinMind.places.size() || !place.is_object()) {
                    break;
                }
                parsed.goblinMind.places[slot++] =
                    KnownPlace{place.value("x", 0), place.value("y", 0),
                                placeKindFromName(place.value("kind", std::string("none"))),
                                place.value("strength", 0)};
            }
        }

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
            parsed.animalGenome =
                parsed.goblin
                    ? genomeFromJson<AnimalGenomeComponent>(record[genomeKey], kGoblinTraits)
                    : (parsed.predator
                           ? genomeFromJson<AnimalGenomeComponent>(record[genomeKey], kPredatorTraits)
                           : genomeFromJson<AnimalGenomeComponent>(record[genomeKey], kHerbivoreTraits));

            if (record.contains("desire")) {
                const auto& desire = record["desire"];
                // Голод, жажда и страх из старых файлов молча
                // игнорируются: они пересчитываются из тела на первом же
                // тике, и хранить их между запусками мира незачем.
                parsed.desire.mating = desire.value("mating", 0.0f);
                parsed.desire.current = desireFromName(desire.value("current", std::string("idle")));
            }
            if (record.contains("goblin_desire")) {
                const auto& desire = record["goblin_desire"];
                parsed.goblinDesire.mating = desire.value("mating", 0);
                parsed.goblinDesire.current =
                    goblinDesireFromName(desire.value("current", std::string("idle")));
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

// Чтение заголовка сохранения, когда файл всё же приходится читать
// целиком (заголовка не оказалось ни в начале, ни в конце — см.
// readSaveHeader ниже).
//
// Разбор событиями, а не nlohmann::json::parse: parse строит дерево на
// весь файл, а файл мира — это десятки мегабайт, и список миров, читая
// их все, укладывал три сотни мегабайт текста в гигабайты узлов ради
// семи полей заголовка. Здесь не строится ничего, а сам разбор
// обрывается, как только "info" прочитан целиком.
class SaveHeaderSax : public nlohmann::json_sax<nlohmann::json> {
public:
    // Прочитан ли заголовок. Всё, что случится с файлом дальше, списку
    // миров уже безразлично: дальше только состояние мира.
    bool headerRead() const { return infoRead_; }
    const std::string& formatTag() const { return formatTag_; }
    const nlohmann::json& info() const { return info_; }

    bool null() override { return field(nullptr); }
    bool boolean(bool value) override { return field(value); }
    bool number_integer(number_integer_t value) override { return field(value); }
    bool number_unsigned(number_unsigned_t value) override { return field(value); }
    bool number_float(number_float_t value, const string_t& /*raw*/) override { return field(value); }
    bool binary(binary_t& /*value*/) override { return field(nullptr); }

    bool string(string_t& value) override {
        if (depth_ == 1 && keys_[1] == "format") {
            formatTag_ = value;
            if (done()) {
                return false;
            }
        }
        return field(value);
    }

    bool key(string_t& value) override {
        if (depth_ <= kTrackedDepth) {
            keys_[depth_] = value;
        }
        return true;
    }

    bool start_object(std::size_t /*elements*/) override {
        const bool isInfo = depth_ == 1 && keys_[1] == "info";
        enter();
        if (isInfo) {
            infoDepth_ = depth_;
        }
        return true;
    }

    bool end_object() override {
        const bool closesInfo = infoDepth_ != 0 && infoDepth_ == depth_;
        leave();
        if (closesInfo) {
            infoDepth_ = 0;
            infoRead_ = true;
            // Ради этой строки всё и написано: дальше в файле мир, а он
            // здесь не нужен.
            return !done();
        }
        return true;
    }

    bool start_array(std::size_t /*elements*/) override {
        enter();
        return true;
    }

    bool end_array() override {
        leave();
        return true;
    }

    // Интерфейс требует остановки; годность прочитанного показывает
    // headerRead(), а не это.
    bool parse_error(std::size_t /*position*/, const std::string& /*token*/,
                      const nlohmann::json::exception& /*error*/) override {
        return false;
    }

private:
    // Глубже второго уровня в заголовке ничего нет: WorldSaveInfo —
    // плоская запись из чисел и строк, и таков её договор в протоколе
    // (shared/world/WorldSaveInfo.hpp). Вложенное глубже пропускается, а
    // не собирается.
    static constexpr int kTrackedDepth = 2;

    template <typename Value>
    bool field(Value&& value) {
        if (infoDepth_ != 0 && depth_ == infoDepth_ && !keys_[depth_].empty()) {
            info_[keys_[depth_]] = std::forward<Value>(value);
        }
        return true;
    }

    bool done() const { return infoRead_ && !formatTag_.empty(); }

    void enter() {
        ++depth_;
        if (depth_ <= kTrackedDepth) {
            keys_[depth_].clear();
        }
    }

    void leave() {
        if (depth_ <= kTrackedDepth) {
            keys_[depth_].clear();
        }
        --depth_;
    }

    int depth_ = 0;
    int infoDepth_ = 0;
    bool infoRead_ = false;
    std::string formatTag_;
    nlohmann::json info_ = nlohmann::json::object();
    std::string keys_[kTrackedDepth + 1];
};

// Заголовок ищется сначала по краям файла и только потом — чтением
// файла целиком.
//
// Разбор событиями дерева не строит, но лексер nlohmann по пути всё
// равно разбирает каждое число и каждую строку: три сотни мегабайт
// сохранений проходятся за девять секунд — ровно то занятое ядро,
// которое было слышно вентилятором при открытии списка миров. Поэтому
// сначала два куска по 64 КиБ:
//
//   начало файла — там заголовок у всего, что пишет нынешний saveWorld
//                  (порядок ключей выбран там руками);
//   конец файла  — там он у файлов прежней записи, где порядок ключей
//                  задавал алфавит nlohmann и "info" оказывался
//                  предпоследним, перед "version".
//
// Край файла — это оборванный JSON, целиком он не разберётся; поэтому
// объект под ключом "info" вырезается по балансу скобок и разбирается
// отдельно. Принимается он, только если это и вправду заголовок мира
// (размеры Области на месте, время записи на месте) — иначе читается
// весь файл, как раньше. Ошибиться проба может, стало быть, лишь в
// сторону лишней работы.
constexpr std::size_t kHeaderProbeWindow = 64 * 1024;

// Похож ли кусок на сохранение мира: метка формата стоит в начале у
// новой записи, номер версии — в конце у прежней.
bool windowLooksLikeSave(const std::string& window) {
    const std::string formatMark = std::string("\"format\":\"") + kFormatTag + "\"";
    const std::string versionMark = std::string("\"version\":") + std::to_string(kWorldSaveFormatVersion);
    return window.find(formatMark) != std::string::npos || window.find(versionMark) != std::string::npos;
}

bool headerFromWindow(const std::string& window, WorldSaveInfo& outInfo) {
    const std::string key = "\"info\":";
    for (std::size_t at = window.find(key); at != std::string::npos; at = window.find(key, at + key.size())) {
        const std::size_t open = window.find('{', at + key.size());
        if (open == std::string::npos) {
            break;
        }

        // Скобки считаем вне строк: в самом заголовке фигурных скобок
        // нет, но кусок мог прийти и из чужого файла.
        std::size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t close = std::string::npos;
        for (std::size_t i = open; i < window.size(); ++i) {
            const char c = window[i];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close == std::string::npos) {
            continue;
        }

        const auto json = nlohmann::json::parse(window.begin() + static_cast<std::ptrdiff_t>(open),
                                                 window.begin() + static_cast<std::ptrdiff_t>(close) + 1, nullptr,
                                                 /*allow_exceptions=*/false);
        if (json.is_discarded() || !json.is_object()) {
            continue;
        }

        WorldSaveInfo info;
        try {
            info = json.get<WorldSaveInfo>();
        } catch (const nlohmann::json::exception&) {
            continue;
        }

        // Заголовок мира узнаётся по себе самому: у настоящего есть и
        // размеры Области, и время записи.
        if (info.area_width <= 0 || info.area_height <= 0 || info.saved_at.empty()) {
            continue;
        }

        outInfo = info;
        return true;
    }
    return false;
}

// Заголовок одного файла. false — файл не открылся, не разобрался или
// сохранением мира не является; причина в outError.
bool readSaveHeader(const std::filesystem::path& path, WorldSaveInfo& outInfo, std::string& outError) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        outError = "could not open the file";
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        outError = "empty file";
        return false;
    }

    const auto windowSize = static_cast<std::size_t>(std::min<std::streamoff>(size, kHeaderProbeWindow));
    std::string window(windowSize, '\0');

    file.seekg(0);
    file.read(&window[0], static_cast<std::streamsize>(windowSize));
    if (windowLooksLikeSave(window) && headerFromWindow(window, outInfo)) {
        return true;
    }

    if (static_cast<std::streamoff>(windowSize) < size) {
        file.clear();
        file.seekg(size - static_cast<std::streamoff>(windowSize));
        file.read(&window[0], static_cast<std::streamsize>(windowSize));
        if (windowLooksLikeSave(window) && headerFromWindow(window, outInfo)) {
            return true;
        }
    }

    // Заголовка на краях нет — придётся читать файл. Разбор событиями
    // хотя бы не строит дерева на весь мир: прежний nlohmann::json::parse
    // укладывал сорок мегабайт текста в гигабайты узлов ради семи полей.
    file.clear();
    file.seekg(0);
    SaveHeaderSax sax;
    // Ответ sax_parse ничего не говорит: разбор обрывается намеренно, и
    // для sax_parse это такая же неудача, как битый файл. Различает их
    // headerRead(). По той же причине strict=false — до конца файла мы
    // не доходим.
    nlohmann::json::sax_parse(file, &sax, nlohmann::json::input_format_t::json, /*strict=*/false);

    if (!sax.headerRead() || sax.formatTag() != kFormatTag) {
        outError = "not a world save";
        return false;
    }

    try {
        outInfo = sax.info().get<WorldSaveInfo>();
    } catch (const nlohmann::json::exception& e) {
        outError = std::string("broken header (") + e.what() + ")";
        return false;
    }
    return true;
}

std::optional<std::string> readCreatedAt(const std::filesystem::path& path) {
    WorldSaveInfo info;
    std::string error;
    if (!readSaveHeader(path, info, error) || info.created_at.empty()) {
        return std::nullopt;
    }
    return info.created_at;
}

// Заголовки уже прочитанных файлов, чтобы список миров не перечитывал
// одно и то же. Годность записи — размер файла и время его записи: мир
// сохранили заново — заголовок перечитается, файл удалили — он просто
// не встретится при обходе каталога. Это не индекс на диске (тот
// неизбежно разъезжается с файлами и переживает их), а память процесса:
// сервер перезапустили — помнить нечего.
//
// Нужно ради тех файлов, у которых заголовок пробой по краям не нашёлся
// (см. readSaveHeader): такой файл читается целиком, а это секунды на
// каждые сорок мегабайт — при том что world_list рассылается далеко не
// только по запросу меню: ещё и при каждом подключении клиента, после
// каждого сохранения мира и после каждой регенерации.
struct CachedHeader {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};
    // Файл, сохранением мира не являющийся, помним тоже: иначе чужой
    // большой .json перечитывался бы при каждом обходе каталога.
    bool isWorldSave = false;
    WorldSaveInfo info;
};

// Каталог сохранений обходят два потока: сетевой (list_worlds,
// delete_world) и GameLoop (после сохранения мира и после регенерации).
std::mutex headerCacheMutex;
std::map<std::filesystem::path, CachedHeader> headerCache;

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

    // Кэш общий на процесс, а обходов каталога может идти два сразу
    // (сетевой поток и GameLoop) — замок на весь обход, благо он теперь
    // короткий.
    std::lock_guard<std::mutex> lock(headerCacheMutex);
    std::set<std::filesystem::path> present;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != kExtension) {
            continue;
        }

        // Размер и время записи берутся из уже прочитанной записи
        // каталога — отдельного обращения к файлу здесь нет.
        std::error_code statEc;
        const auto size = entry.file_size(statEc);
        if (statEc) {
            continue;
        }
        const auto writeTime = entry.last_write_time(statEc);
        if (statEc) {
            continue;
        }

        const auto& path = entry.path();
        present.insert(path);

        const auto cached = headerCache.find(path);
        if (cached == headerCache.end() || cached->second.size != size || cached->second.writeTime != writeTime) {
            CachedHeader header;
            header.size = size;
            header.writeTime = writeTime;
            std::string error;
            header.isWorldSave = readSaveHeader(path, header.info, error);
            if (!header.isWorldSave) {
                // Про один и тот же негодный файл говорим один раз, а не
                // при каждом открытии меню: он же не меняется.
                std::cerr << "WorldSave: '" << path.string() << "' skipped (" << error << ").\n";
            }
            headerCache[path] = std::move(header);
        }

        const CachedHeader& header = headerCache[path];
        if (!header.isWorldSave) {
            continue;
        }

        WorldSaveInfo info = header.info;
        // Источник истины для имени — имя файла, а не поле в нём: файл
        // могли переименовать, и именно по имени файла мир потом
        // загружается.
        info.name = path.stem().string();
        worlds.push_back(std::move(info));
    }

    // Заголовки исчезнувших файлов забываем — иначе кэш хранил бы
    // удалённые миры до перезапуска сервера. Файлы других каталогов (у
    // кэша ключ — полный путь) при этом не трогаем.
    for (auto it = headerCache.begin(); it != headerCache.end();) {
        if (it->first.parent_path() == directory && present.count(it->first) == 0) {
            it = headerCache.erase(it);
        } else {
            ++it;
        }
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

    // Порядок ключей в файле выбран здесь, а не отдан nlohmann: у
    // nlohmann::json объект — это std::map, и ключи ложатся по алфавиту,
    // то есть "entities" (десятки мегабайт) впереди, а "info" (семь полей
    // заголовка) — в самом хвосте. Список миров в меню читает только
    // заголовок, и ради него был вынужден пройти весь файл до конца.
    // Заголовок впереди превращает чтение списка в чтение первых двух
    // сотен байт (см. readSaveHeader). Тот же порядок через
    // nlohmann::ordered_json обошёлся бы дороже самой записи: перенос уже
    // собранного "entities" в объект другого типа — поэлементное
    // копирование всего дерева.
    //
    // Летопись численности пишется рядом с миром, а не в отдельный файл:
    // она описывает жизнь именно этого мира, и разъехаться с ним (удалили
    // мир, осталась летопись) не должна.
    const nlohmann::json entities = buildEntitiesJson(world);

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
        // Значения выводятся прямо в поток, а не через dump() в строку:
        // строка на весь мир — это вторая копия тех же сорока мегабайт.
        file << "{\"format\":" << nlohmann::json(kFormatTag)
             << ",\"version\":" << kWorldSaveFormatVersion
             << ",\"info\":" << nlohmann::json(info)
             << ",\"generation\":" << nlohmann::json(generation)
             << ",\"history\":" << history.toJson()
             << ",\"entities\":" << entities
             << "}";
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
        if (parsed.hasTime || parsed.hasWorldProperties || parsed.hasPlantSpecies || parsed.hasTreeSpecies ||
            parsed.hasBushSpecies || parsed.hasAnimalSpecies) {
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
                world.registry().get<PlantSpeciesComponent>(world.worldEntity()).grasses = parsed.plantSpecies;
            }
            if (parsed.hasTreeSpecies) {
                world.registry().get<PlantSpeciesComponent>(world.worldEntity()).trees = parsed.treeSpecies;
            }
            if (parsed.hasBushSpecies) {
                world.registry().get<PlantSpeciesComponent>(world.worldEntity()).bushes = parsed.bushSpecies;
            }
            if (parsed.hasAnimalSpecies) {
                world.registry().get<AnimalSpeciesComponent>(world.worldEntity()) = parsed.animalSpecies;
            }
            if (parsed.hasGoblinTribes) {
                world.registry().get<GoblinTribesComponent>(world.worldEntity()) = parsed.goblinTribes;
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
        // Метка дерева ложится и на растение, и на семя: по ней и то, и
        // другое живёт по древесным законам. У файлов, записанных до
        // появления деревьев, её просто нет — и весь их растительный мир
        // читается травой, каким и был.
        if (parsed.tree) {
            world.registry().emplace<TreeComponent>(entity);
        }
        // Метка куста — так же, как древесная. У файлов, записанных до
        // появления кустов, её нет, и весь их растительный мир читается
        // травой, каким и был.
        if (parsed.bush) {
            world.registry().emplace<BushComponent>(entity);
        }
        // Ягоды кладутся, даже если их ноль: пустой BerryComponent — это
        // "куст стоит, но обобран", и отличается он от отсутствия куста. У
        // старых файлов ягод нет вовсе, и куст в них появиться не мог.
        if (parsed.hasBerries) {
            world.registry().emplace<BerryComponent>(entity, parsed.berries);
        }
        if (parsed.hasStore) {
            world.registry().emplace<StoreComponent>(entity, parsed.store);
        }
        if (parsed.hasAnimal && parsed.goblin) {
            // Гоблин: то же тело и тот же тип генома, но своё желание и свой
            // тег. Диеты у него нет — он всеяден, и тега для этого не нужно
            // (см. GoblinComponent). Увечий тоже нет: рогов на него никто не
            // наставляет.
            world.registry().emplace<AnimalComponent>(entity, parsed.animal);
            world.registry().emplace<AnimalGenomeComponent>(entity, parsed.animalGenome);
            world.registry().emplace<GoblinDesireComponent>(entity, parsed.goblinDesire);
            world.registry().emplace<IdentityComponent>(entity, IdentityComponent{parsed.identity});
            // Память ног в файле не лежит по той же причине, что и у зверя:
            // шесть последних шагов — это походка, а не мир.
            world.registry().emplace<MovementComponent>(entity);
            world.registry().emplace<GoblinComponent>(entity, parsed.goblinState);
            world.registry().emplace<KnowledgeComponent>(entity, parsed.goblinMind);
            // Руки нужны ВСЕГДА, даже пустые: GoblinSystem выбирает существ
            // по компонентам, и гоблин без рук просто перестал бы жить —
            // молча, и в мире, открытом из старого файла, где ноши ещё не
            // было вовсе.
            world.registry().emplace<CarriedComponent>(entity, parsed.carried);
        } else if (parsed.hasAnimal) {
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

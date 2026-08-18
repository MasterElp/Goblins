#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "config/Config.hpp"
#include "core/GameLoop.hpp"
#include "core/Simulation.hpp"
#include "core/World.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SeedComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "server/ConsoleHotkeyWatcher.hpp"
#include "server/NetworkServer.hpp"
#include "server/PopulationHistory.hpp"
#include "server/WorldSave.hpp"
#include "Version.hpp"

namespace {

// core не знает о JSON/конфигурации (07_TechStack.md, п.6), поэтому
// именно server переносит значения из ServerConfig::TerrainConfig в
// core::TerrainParams перед вызовом generateTerrain.
goblins::TerrainParams toTerrainParams(const goblins::TerrainConfig& config) {
    goblins::TerrainParams params;
    params.featureSize = config.feature_size;
    params.noiseOctaves = config.noise_octaves;
    params.mountainHeight = config.mountain_height;
    params.mountainHardness = config.mountain_hardness;
    params.riverCount = config.river_count;
    params.riverWidth = config.river_width;
    params.riverSinuosity = config.river_sinuosity;
    params.riverDepth = config.river_depth;
    params.pondDepth = config.pond_depth;
    params.mineralsAverage = config.minerals_average;
    params.waterSourceCount = config.water_source_count;
    params.waterSourceDepth = config.water_source_depth;
    params.waterEvaporationRate = config.water_evaporation_rate;
    params.rainIntervalTicks = config.rain_interval_ticks;
    params.rainAmount = config.rain_amount;
    params.soilErosionRate = config.soil_erosion_rate;
    return params;
}

// То же самое для растений: core::PlantParams — не JSON-структура, её
// заполняет server (07_TechStack.md, п.6).
goblins::PlantParams toPlantParams(const goblins::PlantConfig& config) {
    goblins::PlantParams params;
    params.grassSpecies = config.grass_species;
    params.grassCoverage = config.grass_coverage;
    params.mutationRate = config.mutation_rate;
    params.humusDecayPeriod = config.humus_decay_period;
    return params;
}

// И для животных — тем же переносом полей: core::AnimalParams не
// JSON-структура (07_TechStack.md, п.6).
goblins::AnimalParams toAnimalParams(const goblins::AnimalConfig& config) {
    goblins::AnimalParams params;
    params.herbivoreSpecies = config.herbivore_species;
    params.herbivoreCount = config.herbivore_count;
    params.predatorSpecies = config.predator_species;
    params.predatorCount = config.predator_count;
    params.mutationRate = config.mutation_rate;
    return params;
}

// Перенос запроса на генерацию (JSON-структура сервера) в параметры мира
// (core). Сам конвейер генерации — этапы и их порядок — живёт в core
// (core::generateWorld, 02_CorePrinciples.md, п.5): это закон мира, а не
// решение транспортного слоя. Здесь остаётся только перекладывание полей.
goblins::WorldGenParams toWorldGenParams(const goblins::RegenerationRequest& request) {
    goblins::WorldGenParams params;
    params.seed = request.seed;
    params.terrain = toTerrainParams(request.terrain);
    params.boulderCount = request.boulder_count;
    params.plants = toPlantParams(request.plants);
    params.animals = toAnimalParams(request.animals);
    return params;
}

// Мир, в котором нет ни одного размещённого Entity — это мир до первой
// генерации (сервер только поднялся). Сохранять его бессмысленно: в
// списке миров появился бы пустой мир, который можно "загрузить" и
// получить чистую Область.
bool worldIsEmpty(const goblins::World& world) {
    return world.registry().view<const goblins::PositionComponent>().empty();
}

// Размер Области приходит от клиента (панель генерации), поэтому
// проверяется здесь, а не принимается на веру: из ядра эти границы не
// видны, а мир на два тайла или на десять тысяч не сгенерировался бы ни
// осмысленно, ни быстро.
goblins::RegenerationRequest withValidArea(goblins::RegenerationRequest request) {
    request.area_width = std::clamp(request.area_width, goblins::kMinAreaSide, goblins::kMaxAreaSide);
    request.area_height = std::clamp(request.area_height, goblins::kMinAreaSide, goblins::kMaxAreaSide);
    return request;
}

void printWorldStats(const goblins::World& world, int boulderCount) {
    std::size_t waterTiles = 0;
    world.registry().view<goblins::WaterComponent>().each([&](auto) { ++waterTiles; });
    std::cout << "Water tiles: " << waterTiles << "\n";

    // Проверяем на практике правило "непроходимый Entity занимает тайл
    // полностью" (04_WorldModel.md, п.4): у каждого булыжника должен быть
    // уникальный тайл.
    std::set<std::pair<int, int>> occupiedTiles;
    std::size_t placedBoulders = 0;
    world.registry()
        .view<goblins::ImpassableComponent, goblins::PositionComponent>()
        .each([&](auto /*entity*/, const goblins::PositionComponent& pos) {
            ++placedBoulders;
            occupiedTiles.emplace(pos.x, pos.y);
        });

    std::cout << "Boulders placed: " << placedBoulders << " of " << boulderCount << "\n";
    std::cout << "Unique tiles: " << occupiedTiles.size()
               << (occupiedTiles.size() == placedBoulders ? " -- impassability rule holds\n"
                                                            : " -- ERROR: duplicate tiles found!\n");
}

// Виды травы этого мира и их численность — единственное место, где видно,
// какие стратегии выпали при генерации и как они уживаются. Поля генома
// перечисляются не руками, а обходом таблицы черт (kGrassTraits): новая
// черта появится в выводе сама, как и в сохранении и в снапшоте.
void printPlantStats(const goblins::World& world) {
    const auto& archetypes =
        world.registry().get<const goblins::PlantSpeciesComponent>(world.worldEntity()).archetypes;
    if (archetypes.empty()) {
        std::cout << "Grass species: none (plants disabled for this world)\n\n";
        return;
    }

    // Геном есть и у семени (SeedComponent), но семя — ещё не растение,
    // поэтому численность вида считается по PlantComponent, а семена
    // показываются отдельным числом: это семенной банк луга, а не его
    // население.
    std::vector<int> population(archetypes.size(), 0);
    std::size_t plants = 0;
    world.registry().view<const goblins::PlantComponent, const goblins::PlantGenomeComponent>().each(
        [&](const goblins::PlantComponent& /*plant*/, const goblins::PlantGenomeComponent& genome) {
            ++plants;
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < population.size()) {
                ++population[static_cast<std::size_t>(genome.species)];
            }
        });
    std::size_t seeds = 0;
    world.registry().view<const goblins::SeedComponent>().each([&](const goblins::SeedComponent&) { ++seeds; });

    std::size_t humusTiles = 0;
    long long humusMinerals = 0;
    world.registry().view<const goblins::HumusComponent>().each([&](const goblins::HumusComponent& humus) {
        ++humusTiles;
        humusMinerals += humus.minerals;
    });

    std::cout << "Grass species: " << archetypes.size() << ", plants: " << plants << ", seeds: " << seeds
               << ", humus: " << humusTiles
               << " tiles (" << humusMinerals << " minerals waiting to return)\n";
    std::cout << std::fixed;
    for (const auto& archetype : archetypes) {
        std::cout << "  sp" << archetype.species << " n=" << population[static_cast<std::size_t>(archetype.species)];
        for (const auto& trait : goblins::kGrassTraits) {
            std::cout << " " << trait.name << "=" << std::setprecision(4) << archetype.*trait.gene;
        }
        std::cout << "\n";
    }
    std::cout << std::defaultfloat << "\n";
}

// Виды животных этого мира, их поголовье и то, чем оно сейчас занято.
// Желания печатаются рядом с численностью не для красоты: по ним сразу
// видно, чего миру не хватает — стадо, поголовно занятое поиском еды, и
// стадо, которому нечего хотеть, живут в очень разных мирах.
void printAnimalStats(const goblins::World& world) {
    const auto& species = world.registry().get<const goblins::AnimalSpeciesComponent>(world.worldEntity());
    if (species.herbivores.empty() && species.predators.empty()) {
        std::cout << "Animal species: none (no animals in this world)\n\n";
        return;
    }

    // Поголовье считается по диете и виду: индекс вида — свой в каждом из
    // двух списков, поэтому и счётчики раздельные.
    std::vector<int> herbivorePopulation(species.herbivores.size(), 0);
    std::vector<int> herbivoreFemales(species.herbivores.size(), 0);
    std::vector<int> predatorPopulation(species.predators.size(), 0);
    std::vector<int> predatorFemales(species.predators.size(), 0);
    std::size_t animals = 0;
    world.registry()
        .view<const goblins::AnimalComponent, const goblins::AnimalGenomeComponent>()
        .each([&](const auto entity, const goblins::AnimalComponent& animal,
                   const goblins::AnimalGenomeComponent& genome) {
            ++animals;
            const bool predator = world.registry().all_of<goblins::PredatorComponent>(entity);
            auto& population = predator ? predatorPopulation : herbivorePopulation;
            auto& females = predator ? predatorFemales : herbivoreFemales;
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < population.size()) {
                ++population[static_cast<std::size_t>(genome.species)];
                if (animal.sex == goblins::Sex::Female) {
                    ++females[static_cast<std::size_t>(genome.species)];
                }
            }
        });

    std::map<std::string, int> desires;
    world.registry().view<const goblins::DesireComponent>().each([&](const goblins::DesireComponent& desire) {
        ++desires[goblins::desireName(desire.current)];
    });

    std::size_t carcasses = 0;
    float meat = 0.0f;
    world.registry().view<const goblins::CarcassComponent>().each([&](const goblins::CarcassComponent& carcass) {
        ++carcasses;
        meat += carcass.meat;
    });

    std::cout << "Animals: " << animals << " (";
    bool first = true;
    for (const auto& [name, count] : desires) {
        std::cout << (first ? "" : ", ") << name << " " << count;
        first = false;
    }
    std::cout << "), carcasses: " << carcasses << "\n";
    std::cout << std::fixed;

    auto printSpecies = [](const char* prefix, const std::vector<goblins::AnimalGenomeComponent>& archetypes,
                            const std::vector<int>& population, const std::vector<int>& females,
                            std::span<const goblins::AnimalTrait> traits) {
        for (const auto& archetype : archetypes) {
            const auto index = static_cast<std::size_t>(archetype.species);
            if (index >= population.size()) {
                continue;
            }
            std::cout << "  " << prefix << archetype.species << " n=" << population[index] << " (f=" << females[index]
                       << ")";
            for (const auto& trait : traits) {
                std::cout << " " << trait.name << "=" << std::setprecision(4) << archetype.*trait.gene;
            }
            std::cout << "\n";
        }
    };
    printSpecies("hb", species.herbivores, herbivorePopulation, herbivoreFemales, goblins::herbivoreTraits());
    printSpecies("pr", species.predators, predatorPopulation, predatorFemales, goblins::predatorTraits());
    std::cout << std::defaultfloat << "\n";
}

// Единственное место, где стадии generateTerrain видны снаружи core
// (07_TechStack.md, п.6: core сам не делает I/O) — печатает тайминг
// каждой стадии и статистику по рекам/прудам. Если генерация когда-нибудь
// зависнет, последняя напечатанная строка перед зависанием сразу покажет,
// в каком вызове искать; если просто "рек мало", riversPlaced/Requested и
// riverAttemptsUsed/Max сразу показывают, попытки исчерпаны или что-то
// другое.
void printGenerationStats(const goblins::GenerationStats& stats) {
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Terrain generated in " << stats.totalMs << "ms (heightmap " << stats.heightmapMs << "ms, rivers "
               << stats.riverMs << "ms [" << stats.riversPlaced << "/" << stats.riversRequested << " placed, "
               << stats.riverAttemptsUsed << "/" << stats.riverAttemptsMax << " attempts";
    if (stats.riverTimedOut) {
        std::cout << ", TIMED OUT -- hit kRiverStageDeadlineMs, investigate river/noise parameters";
    }
    if (stats.riverPathsCapped > 0) {
        std::cout << ", " << stats.riverPathsCapped << " path(s) hit sample cap";
    }
    std::cout << "], flood-fill " << stats.floodFillMs << "ms, ponds " << stats.pondMs << "ms ["
               << stats.pondComponentsPlaced << " components], " << stats.waterSourcesPlaced
               << " water sources, moisture " << stats.moistureMs << "ms, entities " << stats.entityMs << "ms)\n";
    std::cout << std::defaultfloat;
    if (stats.riverTimedOut || stats.riverPathsCapped > 0) {
        std::cout << "WARNING: river generation hit a safety limit above -- this points at a real bug, please "
                      "report the parameters used.\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    // Версия — первая строка вывода, до чтения конфига: если сервер
    // завис или упал на конфиге, по консоли всё равно видно, какой .exe
    // реально запустился.
    std::cout << "Goblins server v" << goblins::kAppVersion << "\n";

    const std::string configPath = argc > 1 ? argv[1] : goblins::defaultConfigPathNextToExecutable();
    goblins::ensureConfigExists<goblins::ServerConfig>(configPath);
    const auto config = goblins::loadServerConfig(configPath);

    // Каталог сохранённых миров — по одному файлу на мир
    // (server/WorldSave.hpp). Именно он определяет, что покажет клиент в
    // меню выбора мира; если он пуст, клиент попросит сгенерировать
    // новый мир.
    const auto savesDirectory = goblins::resolveSaveDirectory(config.saves_dir);
    std::cout << "Worlds directory: " << savesDirectory.string() << "\n";

    // Мир на первом этапе — одна Область (04_WorldModel.md, п.2), размер —
    // из конфигурации. Новый мир генерируется этим размером; у
    // загруженного мира размер свой, из файла сохранения (World::reset).
    // Почва/водоёмы/булыжники пока не генерируются — сервер только
    // поднимает WebSocket и ждёт команду "start_simulation" от клиента
    // (кнопка Simulation в главном меню). До этого мир пуст: экран World
    // Generation можно открыть и покрутить панель настроек (Regenerate
    // работает независимо от старта симуляции).
    goblins::World world(config.area.width, config.area.height);

    // Игровой цикл: один тик = TimeSystem, затем HydrologySystem (почва и
    // вода — 06_GameLoop.md, п.3: порядок систем фиксирован), затем
    // разрешение очереди команд (06_GameLoop.md, п.2). Интервал тика — из
    // конфигурации. Создаётся до
    // NetworkServer, потому что тот получает ссылку на loop.paused и
    // управляет им напрямую по команде клиента. Стартует на паузе — до
    // start_simulation мир не сгенерирован, тикать нечего.
    goblins::GameLoop loop(world, std::chrono::milliseconds(config.tick_interval_ms));
    loop.paused.store(true);

    // Сетевой слой (07_TechStack.md, п.4): core ничего о нём не знает,
    // NetworkServer — часть server, читает состояние world через
    // публичный интерфейс World. Адрес и порт — из конфигурации.
    // Летопись численности видов — не часть мира и не система тика: мир
    // от неё не меняется (02_CorePrinciples.md, п.1), она лишь смотрит на
    // него после каждого тика. Живёт здесь, рядом с миром и циклом,
    // потому что писать её и сохранять на диск — дело сервера, а не ядра
    // и не сетевого слоя; NetworkServer и WorldSave получают её только на
    // чтение.
    goblins::PopulationHistory populationHistory;

    goblins::NetworkServer network(world, populationHistory, config.host, config.port, loop.paused, config, configPath,
                                    savesDirectory);

    // Перекладывание полей — одной функцией рядом со структурами
    // (shared/config/Config.hpp), а не списком присваиваний здесь: пока
    // список был здесь, в нём не хватало животных, и настройки поголовья
    // из config.json не доезжали до мира вовсе.
    network.setCurrentGenerationConfig(goblins::toRegenerationRequest(config));

    if (!network.start()) {
        return 1;
    }
    std::cout << "WebSocket server listening on ws://" << config.host << ":" << config.port << "\n\n";

    // Вывод "Tick #N" — только каждый 10-й тик и только пока включён:
    // при быстром тике на большом мире печать (и её flush через
    // std::endl) каждый тик заметно отстаёт от реального времени, и
    // визуально кажется, что сервер не реагирует на команды (например,
    // stop_simulation) — хотя пауза уже стоит, просто консоль ещё
    // "доигрывает" накопленный вывод. tickLoggingEnabled переключается
    // горячей клавишей Ctrl+L (см. ниже, ConsoleHotkeyWatcher) —
    // полезно вовсе отключить вывод при активной тонкой настройке
    // параметров с экрана генерации.
    bool tickLoggingEnabled = true;
    constexpr std::uint64_t kTickLogInterval = 10;

    // Законы мира и их порядок — в core (core::addDefaultSystems),
    // не здесь: сервер запускает мир, а не решает, по каким правилам тот
    // живёт.
    goblins::addDefaultSystems(loop);
    loop.onTickComplete = [&](const goblins::World& w) {
        const auto& time = w.registry().get<const goblins::TimeComponent>(w.worldEntity());
        if (tickLoggingEnabled && time.tick % kTickLogInterval == 0) {
            std::cout << "Tick #" << time.tick << std::endl;
        }
        // Летопись — до рассылки: точка этого тика должна уехать клиентам
        // вместе с самим тиком, а не отстать от него на одно сообщение.
        // Пишется она не каждый тик, а раз в PopulationHistory::interval()
        // — решает это она сама.
        populationHistory.record(w);
        // Изменения мира — клиенту после каждого тика: HydrologySystem
        // непрерывно меняет почву/воду, и клиент должен видеть это
        // постепенное изменение вживую, а не только после регенерации.
        // Реально отправлено будет не каждый тик, а не чаще
        // snapshot_interval_ms и только то, что изменилось (см.
        // NetworkServer::publish) — рассылать быстрее, чем клиент
        // способен принимать, значит показывать ему прошлое. Троттлинг
        // вывода в консоль тут ни при чём — это разные потребители одних
        // и тех же данных.
        network.publish();
    };

    // Неблокирующий опрос горячей клавиши Ctrl+L прямо из консоли сервера
    // (сервер без окна — своего цикла событий, как у клиента на raylib, у
    // него нет).
    goblins::ConsoleHotkeyWatcher hotkeys;

    // Цикл не заканчивается сам: мир существует независимо от наблюдателя
    // и тикает, пока жив сервер (02_CorePrinciples.md, п.1).
    loop.run([&]() {
        // Рассылка не только после тика, но и на каждой итерации цикла:
        // на паузе тиков нет вовсе, а подключившийся в этот момент
        // клиент должен получить мир, а не ждать, пока симуляцию
        // запустят. Само сообщение уйдёт, только если есть что
        // отправлять (см. NetworkServer::publish).
        network.publish();

        if (hotkeys.ctrlLPressed()) {
            tickLoggingEnabled = !tickLoggingEnabled;
            std::cout << "Tick console output " << (tickLoggingEnabled ? "enabled" : "disabled")
                       << " (Ctrl+L to toggle).\n";
        }

        // Запуск симуляции по запросу клиента (экран выбора мира) —
        // либо загрузка сохранённого мира, либо генерация нового. Как и
        // регенерация ниже, выполняется здесь, на потоке GameLoop (ECS
        // registry не потокобезопасен между потоками).
        if (const auto start = network.takePendingStartSimulation()) {
            // Генерация или загрузка мира занимает заметное время, а
            // паузой сетевой поток управляет сам и сразу. Если за это
            // время клиент успел прислать stop_simulation (вышел с
            // экрана симуляции), снимать паузу в конце уже нельзя —
            // сравниваем счётчик команд паузы до и после работы.
            const auto pauseCommands = network.pauseCommandCount();
            bool worldReady = false;

            if (start->worldName.empty()) {
                // Новый мир. На диск НЕ пишется автоматически — только
                // играбельное состояние в памяти, с нулевым тиком, пока
                // игрок явно не нажмёт "Save world". Раньше мир сохранялся
                // сразу же, и в списке миров плодились файлы с нулевым
                // тиком от каждого запуска "New world" (включая те, что
                // затем перегенерировали или так и не сыграли ни тика) —
                // список превращался в мусор, который приходилось чистить
                // руками.
                const auto request = withValidArea(network.currentGenerationConfig());
                // std::flush — эта строка обязана попасть в консоль ДО
                // generateTerrain, даже если он зависнет (см. комментарий
                // у printGenerationStats); без явного flush "\n" не
                // гарантирует сброс буфера, если stdout не line-buffered
                // (например, при перенаправлении в файл).
                std::cout << "Creating a new world (seed=" << request.seed << ", area " << request.area_width << "x"
                           << request.area_height << ")...\n"
                           << std::flush;
                // Размер Области — из запроса, а не из config.json: его
                // выбирают на панели генерации вместе с остальным, и мир
                // пересоздаётся целиком (World::reset), как при загрузке
                // сохранения другого размера.
                const auto genStats =
                    goblins::generateWorld(world, request.area_width, request.area_height, toWorldGenParams(request));
                printGenerationStats(genStats);
                printWorldStats(world, request.boulder_count);
                printPlantStats(world);
                printAnimalStats(world);

                // У нового мира летопись своя и начинается с нулевого
                // тика: сшивать её с летописью прежнего мира значило бы
                // врать о том, что было.
                populationHistory.clear();
                populationHistory.record(world);

                network.setCurrentWorldName("");
                worldReady = true;
            } else {
                std::cout << "Loading world '" << start->worldName << "'...\n" << std::flush;

                goblins::RegenerationRequest generation;
                goblins::WorldSaveInfo info;
                std::string error;
                // Летопись загружается вместе с миром (она лежит в том же
                // файле). Если её там нет — старое сохранение, — record
                // ниже заведёт её заново с текущего тика мира.
                if (goblins::loadWorld(world, start->worldName, savesDirectory, generation, populationHistory, info,
                                        error)) {
                    populationHistory.record(world);
                    // Параметры генерации — из файла мира: панель на
                    // экране World Generation должна показывать, чем
                    // сгенерирован именно загруженный мир. Размер Области
                    // при этом берётся у самого мира, а не из его секции
                    // "generation": у сохранений, сделанных до появления
                    // размера в запросе, там стоит значение по умолчанию,
                    // и панель показывала бы 100x100 для мира 400x400.
                    generation.area_width = info.area_width;
                    generation.area_height = info.area_height;
                    network.setCurrentGenerationConfig(generation);
                    network.setCurrentWorldName(info.name);
                    std::cout << "World '" << info.name << "' loaded (tick " << info.tick << ", area "
                               << info.area_width << "x" << info.area_height << ").\n";
                    printWorldStats(world, generation.boulder_count);
                    printPlantStats(world);
                    printAnimalStats(world);
                    network.broadcastNotice("info", "World '" + info.name + "' loaded.");
                    worldReady = true;
                } else {
                    // Мир при неудачной загрузке остаётся нетронутым
                    // (см. loadWorld) — просто не снимаем паузу.
                    std::cerr << "Could not load world: " << error << "\n";
                    network.broadcastNotice("error", "Could not load world: " + error);
                }
            }

            // Мир создан заново (сгенерирован или загружен) — то, что
            // клиент видел до этого, не может служить основой для дельт.
            network.requestFullResync();
            network.publish(/*force=*/true);
            network.broadcastWorldList();

            if (worldReady && network.pauseCommandCount() == pauseCommands) {
                loop.paused.store(false);
                network.broadcastPauseState();
            }
        }

        // Сохранение текущего состояния мира по запросу клиента (кнопка
        // "Save world"). Здесь же, на потоке GameLoop, а не в сетевом
        // колбэке: запись читает весь ECS registry, и делать это
        // параллельно с выполняющимся тиком нельзя.
        if (const auto request = network.takePendingSaveWorld()) {
            if (worldIsEmpty(world)) {
                std::cerr << "Nothing to save: the world has not been generated yet.\n";
                network.broadcastNotice("error", "Nothing to save: the world has not been generated yet.");
            } else {
                std::string name = request->name.empty() ? network.currentWorldName() : request->name;
                if (name.empty()) {
                    // Мир ещё ни разу не сохранялся (например, только что
                    // перегенерирован на экране World Generation) — заводим
                    // для него новый файл, а не переписываем чужой.
                    name = goblins::makeUniqueWorldName(savesDirectory);
                }

                goblins::WorldSaveInfo info;
                std::string error;
                if (goblins::saveWorld(world, network.currentGenerationConfig(), populationHistory, name,
                                        savesDirectory, info, error)) {
                    network.setCurrentWorldName(info.name);
                    std::cout << "World saved as '" << info.name << "' (tick " << info.tick << ").\n";
                    network.broadcastNotice(
                        "info", "World saved as '" + info.name + "' (tick " + std::to_string(info.tick) + ").");
                } else {
                    std::cerr << "Could not save world: " << error << "\n";
                    network.broadcastNotice("error", "Could not save world: " + error);
                }
                // Список миров несёт и имя текущего мира ("current"),
                // поэтому рассылается в любом случае — клиенту нужно
                // увидеть и новый мир в списке, и то, каким он теперь
                // называется.
                network.broadcastWorldList();
            }
        }

        // Регенерация по запросу клиента (панель настроек) — проверяем
        // и выполняем здесь, на потоке GameLoop, до вызова tick(): сама
        // регенерация трогает ECS registry, а это не тот поток, откуда
        // пришёл сетевой запрос (см. NetworkServer::handleClientMessage).
        if (const auto pending = network.takePendingRegeneration()) {
            const auto request = withValidArea(*pending);
            // Печатаем ДО generateTerrain (+ явный flush) — если генерация
            // зависнет (см. GenerationStats::riverTimedOut), эта строка с
            // параметрами запроса всё равно попадёт в консоль и укажет,
            // что именно регенерировалось перед зависанием.
            std::cout << "Regenerating (seed=" << request.seed << ", area " << request.area_width << "x"
                       << request.area_height << ")...\n"
                       << std::flush;
            const auto genStats =
                goblins::generateWorld(world, request.area_width, request.area_height, toWorldGenParams(request));

            // Мир создан заново — летопись прежнего к нему не относится
            // (как и при "New world" выше).
            populationHistory.clear();
            populationHistory.record(world);

            network.setCurrentGenerationConfig(request);
            // Перегенерированный мир — уже не тот, что лежит в файле, из
            // которого он мог быть загружен: до явного "Save world" он
            // безымянный, и сохранение заведёт ему новый файл, а не
            // перезапишет чужой.
            network.setCurrentWorldName("");
            // Карта другая целиком — дельте от прошлой не на что лечь.
            network.requestFullResync();
            network.publish(/*force=*/true);
            network.broadcastWorldList();

            printGenerationStats(genStats);
            printWorldStats(world, request.boulder_count);
            printPlantStats(world);
            printAnimalStats(world);
        }

        return true;
    });

    std::cout << "Simulation stopped.\n";
    network.stop();
    return 0;
}

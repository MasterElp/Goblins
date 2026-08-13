#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <utility>

#include "config/Config.hpp"
#include "core/GameLoop.hpp"
#include "core/World.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/generation/BoulderScatter.hpp"
#include "core/generation/TerrainGenerator.hpp"
#include "core/systems/HydrologySystem.hpp"
#include "core/systems/TimeSystem.hpp"
#include "server/NetworkServer.hpp"
#include "server/WorldSave.hpp"

namespace {

// core не знает о JSON/конфигурации (07_TechStack.md, п.6), поэтому
// именно server переносит значения из ServerConfig::TerrainConfig в
// core::TerrainParams перед вызовом generateTerrain.
goblins::TerrainParams toTerrainParams(const goblins::TerrainConfig& config) {
    goblins::TerrainParams params;
    params.heightNoiseFrequency = config.height_noise_frequency;
    params.rockNoiseFrequency = config.rock_noise_frequency;
    params.compactionNoiseFrequency = config.compaction_noise_frequency;
    params.moistureNoiseFrequency = config.moisture_noise_frequency;
    params.noiseOctaves = config.noise_octaves;
    params.noiseLacunarity = config.noise_lacunarity;
    params.noiseGain = config.noise_gain;
    params.rockHeightBump = config.rock_height_bump;
    params.compactionHeightBump = config.compaction_height_bump;
    params.riverCount = config.river_count;
    params.riverWidth = config.river_width;
    params.riverSinuosity = config.river_sinuosity;
    params.riverDepth = config.river_depth;
    params.riverMaxFlowSpeed = config.river_max_flow_speed;
    params.minPondDepth = config.min_pond_depth;
    params.minPondSize = config.min_pond_size;
    params.maxPondSize = config.max_pond_size;
    params.pondDepthScale = config.pond_depth_scale;
    params.moistureFalloff = config.moisture_falloff;
    params.waterMoistureBoost = config.water_moisture_boost;
    params.rockMoistureReduction = config.rock_moisture_reduction;
    return params;
}

// Генерация мира с нуля: полный сброс (World::reset — пустой registry,
// свежая Область заданного размера, нулевой тик), затем почва/вода и
// булыжники. Один и тот же путь и для нового мира из меню, и для
// Regenerate на экране генерации — сгенерированный мир всегда начинается
// с тика 0, каким бы ни был предыдущий.
goblins::GenerationStats generateWorld(goblins::World& world, const goblins::AreaSize& area,
                                        const goblins::RegenerationRequest& request) {
    world.reset(area.width, area.height);
    const auto stats = generateTerrain(world, request.terrain_seed, toTerrainParams(request.terrain));
    scatterBoulders(world, request.boulder_count, request.boulder_seed);
    return stats;
}

// Мир, в котором нет ни одного размещённого Entity — это мир до первой
// генерации (сервер только поднялся). Сохранять его бессмысленно: в
// списке миров появился бы пустой мир, который можно "загрузить" и
// получить чистую Область.
bool worldIsEmpty(const goblins::World& world) {
    return world.registry().view<const goblins::PositionComponent>().empty();
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
               << (occupiedTiles.size() == placedBoulders ? " -- impassability rule holds\n\n"
                                                            : " -- ERROR: duplicate tiles found!\n\n");
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
               << stats.pondComponentsPlaced << " components], moisture " << stats.moistureMs << "ms, entities "
               << stats.entityMs << "ms)\n";
    std::cout << std::defaultfloat;
    if (stats.riverTimedOut || stats.riverPathsCapped > 0) {
        std::cout << "WARNING: river generation hit a safety limit above -- this points at a real bug, please "
                      "report the parameters used.\n";
    }
}

} // namespace

int main(int argc, char** argv) {
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
    goblins::NetworkServer network(world, config.host, config.port, loop.paused, config, configPath, savesDirectory);

    goblins::RegenerationRequest generationConfig;
    generationConfig.terrain_seed = config.terrain_seed;
    generationConfig.terrain = config.terrain;
    generationConfig.boulder_count = config.boulder_count;
    generationConfig.boulder_seed = config.boulder_seed;
    network.setCurrentGenerationConfig(generationConfig);

    if (!network.start()) {
        return 1;
    }
    std::cout << "WebSocket server listening on ws://" << config.host << ":" << config.port << "\n\n";

    loop.addSystem(goblins::TimeSystem);
    loop.addSystem(goblins::HydrologySystem);
    loop.onTickComplete = [&](const goblins::World& w) {
        const auto& time = w.registry().get<const goblins::TimeComponent>(w.worldEntity());
        std::cout << "Tick #" << time.tick << std::endl;
        // Полный снапшот на каждый тик, а не только номер: HydrologySystem
        // непрерывно меняет почву/воду, и клиент должен видеть это
        // постепенное изменение вживую, а не только после регенерации.
        network.broadcastSnapshot();
    };

    // Отрицательный tick_count в конфигурации — тикать бесконечно (мир
    // существует независимо от наблюдателя, 02_CorePrinciples.md, п.1).
    int ticksRun = 0;
    loop.run([&]() {
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
                // Новый мир. Сразу после генерации он сохраняется — так
                // состояние "мир как он сгенерирован", с нулевым тиком,
                // существует на диске всегда, даже если игрок ни разу не
                // нажмёт "Save world".
                const auto request = network.currentGenerationConfig();
                // std::flush — эта строка обязана попасть в консоль ДО
                // generateTerrain, даже если он зависнет (см. комментарий
                // у printGenerationStats); без явного flush "\n" не
                // гарантирует сброс буфера, если stdout не line-buffered
                // (например, при перенаправлении в файл).
                std::cout << "Creating a new world (terrain_seed=" << request.terrain_seed
                           << ", boulder_seed=" << request.boulder_seed << ")...\n"
                           << std::flush;
                const auto genStats = generateWorld(world, config.area, request);
                printGenerationStats(genStats);
                printWorldStats(world, request.boulder_count);

                goblins::WorldSaveInfo info;
                std::string error;
                const auto name = goblins::makeUniqueWorldName(savesDirectory);
                if (goblins::saveWorld(world, request, name, savesDirectory, info, error)) {
                    network.setCurrentWorldName(info.name);
                    std::cout << "New world saved as '" << info.name << "' (tick " << info.tick << ").\n";
                    network.broadcastNotice("info", "New world '" + info.name + "' created.");
                } else {
                    // Мир сгенерирован и полностью играбелен — не
                    // сохранился только файл. Это не повод не запускать
                    // симуляцию, но игрок должен об этом узнать.
                    network.setCurrentWorldName("");
                    std::cerr << "Could not save the new world: " << error << "\n";
                    network.broadcastNotice("error", "World generated, but not saved: " + error);
                }
                worldReady = true;
            } else {
                std::cout << "Loading world '" << start->worldName << "'...\n" << std::flush;

                goblins::RegenerationRequest generation;
                goblins::WorldSaveInfo info;
                std::string error;
                if (goblins::loadWorld(world, start->worldName, savesDirectory, generation, info, error)) {
                    // Параметры генерации — из файла мира: панель на
                    // экране World Generation должна показывать, чем
                    // сгенерирован именно загруженный мир.
                    network.setCurrentGenerationConfig(generation);
                    network.setCurrentWorldName(info.name);
                    std::cout << "World '" << info.name << "' loaded (tick " << info.tick << ", area "
                               << info.area_width << "x" << info.area_height << ").\n";
                    printWorldStats(world, generation.boulder_count);
                    network.broadcastNotice("info", "World '" + info.name + "' loaded.");
                    worldReady = true;
                } else {
                    // Мир при неудачной загрузке остаётся нетронутым
                    // (см. loadWorld) — просто не снимаем паузу.
                    std::cerr << "Could not load world: " << error << "\n";
                    network.broadcastNotice("error", "Could not load world: " + error);
                }
            }

            network.broadcastSnapshot();
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
                if (goblins::saveWorld(world, network.currentGenerationConfig(), name, savesDirectory, info, error)) {
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
        if (const auto request = network.takePendingRegeneration()) {
            // Печатаем ДО generateTerrain (+ явный flush) — если генерация
            // зависнет (см. GenerationStats::riverTimedOut), эта строка с
            // параметрами запроса всё равно попадёт в консоль и укажет,
            // что именно регенерировалось перед зависанием.
            std::cout << "Regenerating (terrain_seed=" << request->terrain_seed
                       << ", boulder_seed=" << request->boulder_seed << ")...\n"
                       << std::flush;
            const auto genStats = generateWorld(world, config.area, *request);

            network.setCurrentGenerationConfig(*request);
            // Перегенерированный мир — уже не тот, что лежит в файле, из
            // которого он мог быть загружен: до явного "Save world" он
            // безымянный, и сохранение заведёт ему новый файл, а не
            // перезапишет чужой.
            network.setCurrentWorldName("");
            network.broadcastSnapshot();
            network.broadcastWorldList();

            printGenerationStats(genStats);
            printWorldStats(world, request->boulder_count);
        }

        return config.tick_count < 0 || ticksRun++ < config.tick_count;
    });

    std::cout << "Simulation stopped.\n";
    network.stop();
    return 0;
}

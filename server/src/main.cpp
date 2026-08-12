#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

#include "config/Config.hpp"
#include "core/GameLoop.hpp"
#include "core/World.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/generation/BoulderScatter.hpp"
#include "core/generation/TerrainGenerator.hpp"
#include "core/systems/TimeSystem.hpp"
#include "server/NetworkServer.hpp"

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

// Удаляет все Entity, созданные предыдущей генерацией (террейн и
// булыжники), и очищает Area, чтобы сгенерировать заново на том же
// месте. Мировой Entity (TimeComponent) не трогаем — у него нет ни
// SoilComponent, ни ImpassableComponent.
void clearGeneratedEntities(goblins::World& world) {
    std::vector<entt::entity> toDestroy;
    for (const auto entity : world.registry().view<goblins::SoilComponent>()) {
        toDestroy.push_back(entity);
    }
    for (const auto entity : world.registry().view<goblins::ImpassableComponent>()) {
        toDestroy.push_back(entity);
    }
    for (const auto entity : toDestroy) {
        world.registry().destroy(entity);
    }
    world.area().clear();
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

    // Мир на первом этапе — одна Область (04_WorldModel.md, п.2), размер —
    // из конфигурации. Размер Области не меняется живой регенерацией
    // (см. RegenerationRequest) — для этого нужен перезапуск.
    // Почва/водоёмы/булыжники пока не генерируются — сервер только
    // поднимает WebSocket и ждёт команду "start_simulation" от клиента
    // (кнопка Simulation в главном меню). До этого мир пуст: экран World
    // Generation можно открыть и покрутить панель настроек (Regenerate
    // работает независимо от старта симуляции).
    goblins::World world(config.area.width, config.area.height);

    // Игровой цикл: один тик = TimeSystem, затем разрешение очереди команд
    // (06_GameLoop.md, п.2). Интервал тика — из конфигурации. Создаётся до
    // NetworkServer, потому что тот получает ссылку на loop.paused и
    // управляет им напрямую по команде клиента. Стартует на паузе — до
    // start_simulation мир не сгенерирован, тикать нечего.
    goblins::GameLoop loop(world, std::chrono::milliseconds(config.tick_interval_ms));
    loop.paused.store(true);

    // Сетевой слой (07_TechStack.md, п.4): core ничего о нём не знает,
    // NetworkServer — часть server, читает состояние world через
    // публичный интерфейс World. Адрес и порт — из конфигурации.
    goblins::NetworkServer network(world, config.host, config.port, loop.paused, config, configPath);

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
    loop.onTickComplete = [&](const goblins::World& w) {
        const auto& time = w.registry().get<const goblins::TimeComponent>(w.worldEntity());
        std::cout << "Tick #" << time.tick << std::endl;
        network.broadcastTick(time.tick);
    };

    // Отрицательный tick_count в конфигурации — тикать бесконечно (мир
    // существует независимо от наблюдателя, 02_CorePrinciples.md, п.1).
    int ticksRun = 0;
    loop.run([&]() {
        // Запуск симуляции по запросу клиента (кнопка Simulation) —
        // первая генерация мира текущим конфигом и снятие паузы. Как и
        // регенерация ниже, выполняется здесь, на потоке GameLoop (ECS
        // registry не потокобезопасен между потоками).
        if (network.takePendingStartSimulation()) {
            const auto request = network.currentGenerationConfig();
            // std::flush — эта строка обязана попасть в консоль ДО
            // generateTerrain, даже если он зависнет (см. комментарий у
            // printGenerationStats); без явного flush "\n" не гарантирует
            // сброс буфера, если stdout не line-buffered (например, при
            // перенаправлении в файл).
            std::cout << "Simulation starting (terrain_seed=" << request.terrain_seed
                       << ", boulder_seed=" << request.boulder_seed << ")...\n"
                       << std::flush;
            const auto genStats = generateTerrain(world, request.terrain_seed, toTerrainParams(request.terrain));
            scatterBoulders(world, request.boulder_count, request.boulder_seed);

            network.broadcastSnapshot();

            printGenerationStats(genStats);
            printWorldStats(world, request.boulder_count);

            loop.paused.store(false);
            network.broadcastPauseState();
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
            clearGeneratedEntities(world);
            const auto genStats = generateTerrain(world, request->terrain_seed, toTerrainParams(request->terrain));
            scatterBoulders(world, request->boulder_count, request->boulder_seed);

            network.setCurrentGenerationConfig(*request);
            network.broadcastSnapshot();

            printGenerationStats(genStats);
            printWorldStats(world, request->boulder_count);
        }

        return config.tick_count < 0 || ticksRun++ < config.tick_count;
    });

    std::cout << "Simulation stopped.\n";
    network.stop();
    return 0;
}

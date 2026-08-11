#include <chrono>
#include <iostream>
#include <set>
#include <utility>

#include "config/Config.hpp"
#include "core/GameLoop.hpp"
#include "core/World.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/generation/BoulderScatter.hpp"
#include "core/systems/TimeSystem.hpp"
#include "server/NetworkServer.hpp"

int main(int argc, char** argv) {
    const std::string configPath = argc > 1 ? argv[1] : goblins::defaultConfigPathNextToExecutable();
    goblins::ensureConfigExists<goblins::ServerConfig>(configPath);
    const auto config = goblins::loadServerConfig(configPath);

    // Мир на первом этапе — одна Область (04_WorldModel.md, п.2), размер —
    // из конфигурации.
    goblins::World world(config.area.width, config.area.height);

    scatterBoulders(world, config.boulder_count, config.boulder_seed);

    std::cout << "Area: " << world.area().width() << "x" << world.area().height() << "\n";

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

    std::cout << "Boulders placed: " << placedBoulders << " of " << config.boulder_count << "\n";
    std::cout << "Unique tiles: " << occupiedTiles.size()
               << (occupiedTiles.size() == placedBoulders ? " -- impassability rule holds\n\n"
                                                            : " -- ERROR: duplicate tiles found!\n\n");

    // Игровой цикл: один тик = TimeSystem, затем разрешение очереди команд
    // (06_GameLoop.md, п.2). Интервал тика — из конфигурации. Создаётся до
    // NetworkServer, потому что тот получает ссылку на loop.paused и
    // управляет им напрямую по команде клиента.
    goblins::GameLoop loop(world, std::chrono::milliseconds(config.tick_interval_ms));

    // Сетевой слой (07_TechStack.md, п.4): core ничего о нём не знает,
    // NetworkServer — часть server, читает состояние world через
    // публичный интерфейс World. Адрес и порт — из конфигурации.
    goblins::NetworkServer network(world, config.host, config.port, loop.paused);
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
    loop.run([&]() { return config.tick_count < 0 || ticksRun++ < config.tick_count; });

    std::cout << "Simulation stopped.\n";
    network.stop();
    return 0;
}

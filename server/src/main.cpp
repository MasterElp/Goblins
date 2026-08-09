#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <utility>

#include "core/GameLoop.hpp"
#include "core/World.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/generation/BoulderScatter.hpp"
#include "core/systems/TimeSystem.hpp"
#include "server/NetworkServer.hpp"

int main(int argc, char** argv) {
    int tickCount = 10;
    if (argc > 1) {
        tickCount = std::atoi(argv[1]);
    }

    int port = 9002;
    if (argc > 2) {
        port = std::atoi(argv[2]);
    }

    // Мир на первом этапе — одна Область 100x100 (04_WorldModel.md, п.2).
    goblins::World world;

    constexpr int boulderCount = 40;
    scatterBoulders(world, boulderCount, /*seed=*/12345);

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

    std::cout << "Boels: " << placedBoulders << " of " << boulderCount << "\n";
    std::cout << "Unic: " << occupiedTiles.size()
               << (occupiedTiles.size() == placedBoulders ? " — cool\n\n"
                                                            : " — error: dublicats!\n\n");

    // Сетевой слой (07_TechStack.md, п.4): core ничего о нём не знает,
    // NetworkServer — часть server, читает состояние world через
    // публичный интерфейс World.
    goblins::NetworkServer network(world, port);
    if (!network.start()) {
        return 1;
    }
    std::cout << "WebSocket-server hear ws://localhost:" << port << "\n\n";

    // Игровой цикл: один тик = TimeSystem, затем разрешение очереди команд
    // (06_GameLoop.md, п.2).
    goblins::GameLoop loop(world, std::chrono::milliseconds(200));
    loop.addSystem(goblins::TimeSystem);
    loop.onTickComplete = [&](const goblins::World& w) {
        const auto& time = w.registry().get<const goblins::TimeComponent>(w.worldEntity());
        std::cout << "Tick #" << time.tick << std::endl;
        network.broadcastTick(time.tick);
    };

    int ticksRun = 0;
    loop.run([&]() { return ticksRun++ < tickCount; });

    std::cout << "Simulation done after " << tickCount << " ticks.\n";
    network.stop();
    return 0;
}

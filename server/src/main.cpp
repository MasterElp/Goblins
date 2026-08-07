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

int main(int argc, char** argv) {
    int tickCount = 10;
    if (argc > 1) {
        tickCount = std::atoi(argv[1]);
    }

    // Мир на первом этапе — одна Область 100x100 (04_WorldModel.md, п.2).
    goblins::World world;

    constexpr int boulderCount = 40;
    scatterBoulders(world, boulderCount, /*seed=*/12345);

    std::cout << "Область: " << world.area().width() << "x" << world.area().height() << "\n";

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
            std::cout << "  Булыжник на (" << pos.x << ", " << pos.y << ")\n";
        });

    std::cout << "Размещено булыжников: " << placedBoulders << " из " << boulderCount << "\n";
    std::cout << "Уникальных тайлов: " << occupiedTiles.size()
               << (occupiedTiles.size() == placedBoulders ? " — правило непроходимости соблюдено\n\n"
                                                            : " — ОШИБКА: есть повторы тайлов!\n\n");

    // Игровой цикл: один тик = TimeSystem, затем разрешение очереди команд
    // (06_GameLoop.md, п.2). Порядок систем пока состоит из одной системы —
    // остальные появятся на следующих этапах.
    goblins::GameLoop loop(world, std::chrono::milliseconds(200));
    loop.addSystem(goblins::TimeSystem);
    loop.onTickComplete = [](const goblins::World& w) {
        const auto& time = w.registry().get<const goblins::TimeComponent>(w.worldEntity());
        std::cout << "Tick #" << time.tick << std::endl;
    };

    int ticksRun = 0;
    loop.run([&]() { return ticksRun++ < tickCount; });

    std::cout << "Симуляция остановлена после " << tickCount << " тиков.\n";
    return 0;
}

#include "core/generation/BoulderScatter.hpp"

#include <random>

#include "core/components/ImpassableComponent.hpp"
#include "core/components/WaterComponent.hpp"

namespace goblins {

namespace {

// Тайл с водой (рекой или прудом) — терраформирующий Entity на нём несёт
// WaterComponent (см. TerrainGenerator). Булыжник туда ставить нельзя.
bool hasWater(const World& world, int x, int y) {
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (world.registry().all_of<WaterComponent>(entity)) {
            return true;
        }
    }
    return false;
}

} // namespace

void scatterBoulders(World& world, int count, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> xDist(0, world.area().width() - 1);
    std::uniform_int_distribution<int> yDist(0, world.area().height() - 1);

    int placed = 0;
    int attempts = 0;
    // Ограничение попыток: если карта почти заполнена непроходимыми
    // объектами, случайный поиск свободного тайла не должен уйти в
    // бесконечный цикл.
    const int maxAttempts = count * 100;

    while (placed < count && attempts < maxAttempts) {
        ++attempts;

        const int x = xDist(rng);
        const int y = yDist(rng);

        // Непроходимый Entity занимает тайл полностью — если тайл уже
        // занят, пробуем другой (04_WorldModel.md, п.4).
        if (world.area().isBlocked(x, y) || hasWater(world, x, y)) {
            continue;
        }

        const auto boulder = world.registry().create();
        // Непроходимость — до размещения: World читает её у самого Entity
        // (см. World::place), а не получает флагом.
        world.registry().emplace<ImpassableComponent>(boulder);
        world.place(boulder, x, y);

        ++placed;
    }
}

} // namespace goblins

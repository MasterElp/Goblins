#include "core/generation/TreeSeeding.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "core/Random.hpp"
#include "core/Scale.hpp"
#include "core/Trees.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// Ограничение попыток, как в BoulderScatter и GrassSeeding. Множитель выше
// травяного: подходящих для дерева мест на карте немного (богатая земля
// лежит пятнами), и случайный поиск чаще уходит впустую.
constexpr int kAttemptMultiplier = 60;

// Суше половины своей потребности дерево не сажаем — тот же порог, что у
// травы (kSeedingMinSupply в GrassSeeding.cpp).
constexpr int kSeedingMinSupply = 500;

entt::entity terrainEntityAt(World& world, int x, int y) {
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (world.registry().all_of<SoilComponent>(entity)) {
            return entity;
        }
    }
    return entt::null;
}

bool hasPlant(World& world, int x, int y) {
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (world.registry().all_of<PlantComponent>(entity)) {
            return true;
        }
    }
    return false;
}

bool treeNear(World& world, int x, int y) {
    for (int dy = -kTreeSpacing; dy <= kTreeSpacing; ++dy) {
        for (int dx = -kTreeSpacing; dx <= kTreeSpacing; ++dx) {
            if (!world.area().inBounds(x + dx, y + dy)) {
                continue;
            }
            for (const auto entity : world.area().cellAt(x + dx, y + dy).entities) {
                if (world.registry().all_of<PlantComponent, TreeComponent>(entity)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

void seedTrees(World& world, const PlantParams& params, unsigned seed) {
    const int width = world.area().width();
    const int height = world.area().height();
    const auto cellCount = static_cast<long long>(width) * height;
    if (cellCount == 0) {
        return;
    }

    auto species = makeTreeSpecies(params.treeSpecies, static_cast<std::uint64_t>(seed));
    auto& speciesComponent = world.registry().get<PlantSpeciesComponent>(world.worldEntity());
    speciesComponent.trees = species;
    if (species.empty()) {
        return;
    }

    const int target = static_cast<int>(static_cast<long long>(params.treeCoverage) * cellCount / kFull);
    if (target <= 0) {
        return;
    }
    const int maxAttempts = target * kAttemptMultiplier;

    // Расклад бюджета черт дробный — это генерация, а не состояние мира
    // (core/Scale.hpp), поэтому целая настройка переводится в долю здесь.
    const float mutationRate = static_cast<float>(params.mutationRate) / kFull;

    std::uint64_t state = mixSeed(seed, 0x7BEE0F5EED11C0DEull);
    int planted = 0;
    int attempts = 0;
    while (planted < target && attempts < maxAttempts) {
        ++attempts;

        const int x = static_cast<int>(randomUnit(state) * static_cast<float>(width)) % width;
        const int y = static_cast<int>(randomUnit(state) * static_cast<float>(height)) % height;

        if (world.area().isBlocked(x, y) || hasPlant(world, x, y) || treeNear(world, x, y)) {
            continue;
        }
        const auto terrain = terrainEntityAt(world, x, y);
        if (terrain == entt::null) {
            continue;
        }
        const auto* water = world.registry().try_get<const WaterComponent>(terrain);
        const int waterDepth = water != nullptr ? water->depth : 0;

        const std::size_t speciesIndex =
            static_cast<std::size_t>(randomUnit(state) * static_cast<float>(species.size())) % species.size();
        const auto& archetype = species[speciesIndex];
        // Тот же разброс внутри вида, что и у травы: генетическая
        // вариативность существует с первого тика, а не появляется через
        // поколения.
        const PlantGenomeComponent genome =
            mutateTreeGenome(archetype, archetype, mutationRate, mixSeed(state, static_cast<std::uint64_t>(planted)));

        if (waterDepth > genome.waterTolerance) {
            continue;
        }

        const int moisture = world.registry().get<const SoilComponent>(terrain).moisture;
        const int supply =
            genome.moistureNeed > 0 ? std::min(kFull, moisture * kFull / genome.moistureNeed) : kFull;
        if (supply < kSeedingMinSupply) {
            continue;
        }

        // Хватит ли земли вокруг на взрослое дерево. Это единственное, что
        // делает рощи рощами: крупицы розданы пятнами, и там, где их мало,
        // дерева не будет ни сейчас, ни потом (core/Trees.hpp).
        int rootSupply = 0;
        for (int dy = -kTreeRootRadius; dy <= kTreeRootRadius; ++dy) {
            for (int dx = -kTreeRootRadius; dx <= kTreeRootRadius; ++dx) {
                if (!world.area().inBounds(x + dx, y + dy)) {
                    continue;
                }
                const auto neighbour = terrainEntityAt(world, x + dx, y + dy);
                if (neighbour != entt::null) {
                    rootSupply += world.registry().get<const SoilComponent>(neighbour).minerals;
                }
            }
        }
        if (rootSupply < kTreeMinerals) {
            continue;
        }

        // Стартовый лес не должен быть строем ровесников: возраст случайный,
        // а размер — тот, до которого дерево успело бы дорасти к этому
        // возрасту. Иначе вся роща умирала бы одним тиком через десятки
        // тысяч тиков после начала мира.
        PlantComponent plant;
        plant.age = static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(std::max(1, genome.maxAge))));
        plant.growth = std::min(kFull, plant.age * genome.growthRate);

        // Крупицы дерево берёт не из воздуха: столько, сколько нужно на его
        // нынешний размер, и ровно оттуда, откуда потом будет тянуть корнями
        // — от своей клетки наружу.
        int need = kTreeMinerals * plant.growth / kFull;
        for (int radius = 0; radius <= kTreeRootRadius && need > 0; ++radius) {
            for (int dy = -radius; dy <= radius && need > 0; ++dy) {
                for (int dx = -radius; dx <= radius && need > 0; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius || !world.area().inBounds(x + dx, y + dy)) {
                        continue;
                    }
                    const auto neighbour = terrainEntityAt(world, x + dx, y + dy);
                    if (neighbour == entt::null) {
                        continue;
                    }
                    auto& soil = world.registry().get<SoilComponent>(neighbour);
                    const int taken = std::min(need, soil.minerals);
                    soil.minerals -= taken;
                    plant.minerals += taken;
                    need -= taken;
                }
            }
        }
        // Чем прокормились корни, тем дерево и выросло — тот же закон, что и
        // в каждый тик его жизни (PlantSystem).
        plant.growth = std::min(plant.growth, plant.minerals * kFull / kTreeMinerals);

        const auto entity = world.registry().create();
        world.registry().emplace<PlantComponent>(entity, plant);
        world.registry().emplace<PlantGenomeComponent>(entity, genome);
        world.registry().emplace<TreeComponent>(entity);
        world.place(entity, x, y);

        ++planted;
    }
}

// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendTreeSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Tree seeding";
    out.push_back({g, "kAttemptMultiplier", static_cast<float>(kAttemptMultiplier)});
    out.push_back({g, "kSeedingMinSupply", static_cast<float>(kSeedingMinSupply)});
}

} // namespace goblins

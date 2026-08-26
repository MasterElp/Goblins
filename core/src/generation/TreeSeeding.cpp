#include "core/generation/TreeSeeding.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

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
#include "core/generation/Nest.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// Докуда куртина расходится от своего центра. Не форма и не предел рощи:
// сажать перестают, когда набрана доля вида или когда земля вокруг
// кончилась (plantTree) — а этим числом просто ограничен обход гнезда
// (core/generation/Nest.hpp), чтобы он не пошёл по всей карте кольцами,
// ничего не находя.
constexpr int kGroveRadius = 30;

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

// Сколько крупиц лежит в земле под будущими корнями.
int rootMinerals(World& world, int x, int y) {
    int total = 0;
    for (int dy = -kTreeRootRadius; dy <= kTreeRootRadius; ++dy) {
        for (int dx = -kTreeRootRadius; dx <= kTreeRootRadius; ++dx) {
            if (!world.area().inBounds(x + dx, y + dy)) {
                continue;
            }
            const auto neighbour = terrainEntityAt(world, x + dx, y + dy);
            if (neighbour != entt::null) {
                total += world.registry().get<const SoilComponent>(neighbour).minerals;
            }
        }
    }
    return total;
}

// Посадить дерево вида archetype в клетку (x, y), если оно там уместится и
// прокормится. false — не уместилось; вызывающая сторона просто идёт
// дальше, это не ошибка.
bool plantTree(World& world, int x, int y, const PlantGenomeComponent& archetype, float mutationRate,
               std::uint64_t& state) {
    if (world.area().isBlocked(x, y) || hasPlant(world, x, y) || treeNear(world, x, y)) {
        return false;
    }
    const auto terrain = terrainEntityAt(world, x, y);
    if (terrain == entt::null) {
        return false;
    }

    // Тот же разброс внутри вида, что и у травы: генетическая вариативность
    // существует с первого тика, а не появляется через поколения.
    const PlantGenomeComponent genome = mutateTreeGenome(archetype, archetype, mutationRate, nextState(state));

    const auto* water = world.registry().try_get<const WaterComponent>(terrain);
    if ((water != nullptr ? water->depth : 0) > genome.waterTolerance) {
        return false;
    }

    const int moisture = world.registry().get<const SoilComponent>(terrain).moisture;
    const int supply = genome.moistureNeed > 0 ? std::min(kFull, moisture * kFull / genome.moistureNeed) : kFull;
    if (supply < kSeedingMinSupply) {
        return false;
    }

    // Хватит ли земли вокруг на взрослое дерево. Это единственное, что
    // делает рощи рощами: крупицы розданы пятнами, и там, где их мало,
    // дерева не будет ни сейчас, ни потом (core/Trees.hpp). Здесь же это и
    // край куртины: посаженные соседи уже выбрали свою землю, и куртина
    // сама останавливается там, где земля кончилась.
    if (rootMinerals(world, x, y) < kTreeMinerals) {
        return false;
    }

    // Стартовый лес не должен быть строем ровесников: возраст случайный, а
    // размер — тот, до которого дерево успело бы дорасти к этому возрасту.
    // Иначе вся куртина умирала бы одним тиком через десятки тысяч тиков
    // после начала мира.
    PlantComponent plant;
    plant.age = static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(std::max(1, genome.maxAge))));
    plant.growth = std::min(kFull, plant.age * genome.growthRate);

    // Крупицы дерево берёт не из воздуха: столько, сколько нужно на его
    // нынешний размер, и ровно оттуда, откуда потом будет тянуть корнями —
    // от своей клетки наружу.
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
    // Чем прокормились корни, тем дерево и выросло — тот же закон, что и в
    // каждый тик его жизни (PlantSystem).
    plant.growth = std::min(plant.growth, plant.minerals * kFull / kTreeMinerals);

    const auto entity = world.registry().create();
    world.registry().emplace<PlantComponent>(entity, plant);
    world.registry().emplace<PlantGenomeComponent>(entity, genome);
    world.registry().emplace<TreeComponent>(entity);
    world.place(entity, x, y);
    return true;
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

    // Расклад бюджета черт дробный — это генерация, а не состояние мира
    // (core/Scale.hpp), поэтому целая настройка переводится в долю здесь.
    const float mutationRate = static_cast<float>(params.mutationRate) / kFull;

    std::uint64_t state = mixSeed(seed, 0x7BEE0F5EED11C0DEull);

    // Каждый вид высаживается ОДНОЙ плотной куртиной, а не рассыпается по
    // карте поодиночке: рассыпанное дерево почти всегда оказывалось
    // единственным на несколько десятков клеток — семя летит шесть клеток
    // (core/Trees.hpp), соседа в этом круге у него нет, и весь мир начинался
    // как поле одиночек, которые тысячи тиков сползались бы в рощи, если бы
    // успели.
    //
    // Сама раскладка кольцами — общий закон (core/generation/Nest.hpp): им же
    // расставляются кусты, стада и племена. Здесь остаётся только то, что и
    // правда про деревья: чем годен центр и как сажается одно дерево. Плотной
    // куртина при этом выходит настолько, насколько позволяет земля, и ровным
    // кругом не становится: её край рвут камни, вода и, главное, минералы —
    // где их под корнями мало, там дерева не будет.
    const int perSpecies = std::max(1, target / static_cast<int>(species.size()));
    for (const auto& archetype : species) {
        // Годность клетки под ЦЕНТР куртины строже, чем под соседнее дерево:
        // придирчивость этой проверки и делает выбор места осмысленным —
        // центр всегда оказывается на богатой земле у воды. Край же куртины
        // отрежет сам plantTree там, где земля кончится.
        const auto suitableCenter = [&](int x, int y) {
            if (world.area().isBlocked(x, y) || hasPlant(world, x, y) || treeNear(world, x, y)) {
                return false;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            return terrain != entt::null && rootMinerals(world, x, y) >= kTreeMinerals;
        };
        seedNest(
            world.area(), perSpecies, kGroveRadius, suitableCenter,
            [&](int x, int y) { return plantTree(world, x, y, archetype, mutationRate, state); }, state);
    }
}

// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendTreeSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Tree seeding";
    // Перебор в поисках центра — уже не свой, а общий с прочими гнёздами
    // (core/generation/Nest.hpp); показан здесь, потому что смотреть на него
    // надо рядом с радиусом куртины.
    out.push_back({g, "kNestCenterAttempts", static_cast<float>(kNestCenterAttempts)});
    out.push_back({g, "kGroveRadius", static_cast<float>(kGroveRadius)});
    out.push_back({g, "kSeedingMinSupply", static_cast<float>(kSeedingMinSupply)});
}

} // namespace goblins

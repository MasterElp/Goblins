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
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// Сколько случайных клеток перебирается в поисках места под куртину. Не
// множитель от числа деревьев, как в BoulderScatter: ищется одна клетка на
// вид, а не место каждому дереву, — зато ищется придирчиво (нужна и вода, и
// богатая земля), и на скупой карте перебор может уйти впустую весь.
constexpr int kCenterAttempts = 3000;

// Докуда куртина расходится от своего центра. Не форма и не предел рощи:
// сажать перестают, когда набрана доля вида или когда земля вокруг
// кончилась (см. seedTrees) — а этим числом просто ограничен обход, чтобы
// он не пошёл по всей карте кольцами, ничего не находя.
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

    // Каждый вид высаживается ОДНОЙ плотной куртиной в случайном месте, а
    // не рассыпается по карте поодиночке.
    //
    // Так у вида с первого тика есть место, где он живёт, а не редкая сыпь
    // от края до края. Рассыпанное же дерево почти всегда оказывалось
    // единственным на несколько десятков клеток: семя летит шесть клеток
    // (core/Trees.hpp), соседа в этом круге у него нет, и весь мир
    // начинался как поле одиночек, которые тысячи тиков сползались бы в
    // рощи — если бы успели.
    //
    // Плотно — значит настолько, насколько позволяет земля: сажаем кольцами
    // от центра наружу, пока не набрана доля вида или пока кольца не
    // перестанут что-либо давать. Ровным кругом куртина от этого не
    // становится: её край рвут камни, вода и, главное, минералы — где их
    // под корнями мало, там дерева не будет (plantTree выше).
    const int perSpecies = std::max(1, target / static_cast<int>(species.size()));
    for (const auto& archetype : species) {
        // Центр куртины — первая случайная клетка, куда вид вообще может
        // сесть. Придирчивость проверки здесь и делает выбор места
        // осмысленным: центр всегда оказывается на богатой земле у воды.
        int centerX = -1;
        int centerY = -1;
        for (int attempt = 0; attempt < kCenterAttempts && centerX < 0; ++attempt) {
            const int x = static_cast<int>(randomUnit(state) * static_cast<float>(width)) % width;
            const int y = static_cast<int>(randomUnit(state) * static_cast<float>(height)) % height;
            if (world.area().isBlocked(x, y) || hasPlant(world, x, y) || treeNear(world, x, y)) {
                continue;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            if (terrain == entt::null || rootMinerals(world, x, y) < kTreeMinerals) {
                continue;
            }
            centerX = x;
            centerY = y;
        }
        if (centerX < 0) {
            continue; // виду не нашлось места вовсе — мир для него слишком беден
        }

        int planted = 0;
        if (plantTree(world, centerX, centerY, archetype, mutationRate, state)) {
            ++planted;
        }
        // Кольцо за кольцом наружу. Обход каждого кольца начинается со
        // случайной его клетки: иначе куртина заполнялась бы всегда с
        // одного угла и на скупой земле получалась бы полумесяцем, глядящим
        // в одну и ту же сторону во всех мирах.
        std::vector<std::pair<int, int>> ring;
        for (int radius = 1; radius <= kGroveRadius && planted < perSpecies; ++radius) {
            ring.clear();
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius) {
                        continue;
                    }
                    const int x = centerX + dx;
                    const int y = centerY + dy;
                    if (world.area().inBounds(x, y)) {
                        ring.emplace_back(x, y);
                    }
                }
            }
            if (ring.empty()) {
                continue;
            }
            const std::size_t start =
                static_cast<std::size_t>(randomBelow(state, static_cast<std::uint64_t>(ring.size())));
            for (std::size_t n = 0; n < ring.size() && planted < perSpecies; ++n) {
                const auto& cell = ring[(start + n) % ring.size()];
                if (plantTree(world, cell.first, cell.second, archetype, mutationRate, state)) {
                    ++planted;
                }
            }
        }
    }
}

// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendTreeSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Tree seeding";
    out.push_back({g, "kCenterAttempts", static_cast<float>(kCenterAttempts)});
    out.push_back({g, "kGroveRadius", static_cast<float>(kGroveRadius)});
    out.push_back({g, "kSeedingMinSupply", static_cast<float>(kSeedingMinSupply)});
}

} // namespace goblins

#include "core/generation/BushSeeding.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/Berries.hpp"
#include "core/Diagnostics.hpp"
#include "core/Random.hpp"
#include "core/PlantKind.hpp"
#include "core/Scale.hpp"
#include "core/components/BerryComponent.hpp"
#include "core/components/BushComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/generation/Nest.hpp"
#include "core/generation/PlantGenetics.hpp"

namespace goblins {

namespace {

// Суше половины своей потребности куст не сажаем — тот же порог, что у травы
// и у дерева (kSeedingMinSupply в GrassSeeding.cpp и TreeSeeding.cpp).
constexpr int kSeedingMinSupply = 500;

// Сколько кустов в одной купе. Дюжина — это ягодник, к которому есть смысл
// идти: полный куст держит kBerryMax ягод (core/Berries.hpp), и купа кормит
// небольшую группу, не кормя племени. Меньше — не место, а находка на один
// раз; больше — заросли, в которых незачем помнить, где именно ты ел.
constexpr int kBushPatchSize = 12;

// Докуда купа расходится от центра. Заметно теснее рощи (kGroveRadius = 30):
// ягодник должен читаться на карте как пятно, а не как редкая сыпь по
// четверти области. Число ограничивает обход гнезда, а не задаёт форму —
// форму рвут вода, камень и бедная земля.
constexpr int kBushPatchRadius = 6;

// Сколько крупиц должно лежать в клетке, чтобы она годилась под ЦЕНТР купы.
// Куст с них начнёт завязывать ягоды (kBerriesPerGrain, core/Berries.hpp), и
// центр, выбранный по этому признаку, ставит ягодник на богатую землю сам —
// без правила "ягодники растут там-то".
constexpr int kBushCenterMinerals = kBerriesPerGrain;

entt::entity terrainEntityAt(World& world, int x, int y) {
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (world.registry().all_of<SoilComponent>(entity)) {
            return entity;
        }
    }
    return entt::null;
}

bool hasPlant(const World& world, int x, int y) {
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (world.registry().all_of<PlantComponent>(entity)) {
            return true;
        }
    }
    return false;
}

// Посадить куст вида archetype в клетку (x, y), если он там уместится и
// приживётся. false — не уместился; вызывающая сторона просто идёт дальше.
bool plantBush(World& world, int x, int y, const PlantGenomeComponent& archetype, float mutationRate, int lifespan,
               std::uint64_t& state) {
    if (world.area().isBlocked(x, y) || hasPlant(world, x, y)) {
        return false;
    }
    const auto terrain = terrainEntityAt(world, x, y);
    if (terrain == entt::null) {
        return false;
    }

    // Тот же разброс внутри вида, что у травы и дерева: генетическая
    // вариативность существует с первого тика, а не появляется через
    // поколения.
    const PlantGenomeComponent genome = mutateBushGenome(archetype, archetype, mutationRate, nextState(state));

    const auto* water = world.registry().try_get<const WaterComponent>(terrain);
    if ((water != nullptr ? water->depth : 0) > genome.waterTolerance) {
        return false;
    }

    auto& soil = world.registry().get<SoilComponent>(terrain);
    const int supply = genome.moistureNeed > 0 ? std::min(kFull, soil.moisture * kFull / genome.moistureNeed) : kFull;
    if (supply < kSeedingMinSupply) {
        return false;
    }

    // Стартовый ягодник не должен быть строем ровесников: возраст случайный
    // в пределах до созревания, размер — тот, до которого куст успел бы
    // дорасти. Иначе купа зацвела бы и умерла одной волной.
    PlantComponent plant;
    plant.age = static_cast<int>(
        randomBelow(state, static_cast<std::uint64_t>(std::max(1, plantMaturityAgeOf(genome, lifespan)))));
    plant.growth = std::min(kFull, plant.age * genome.growthRate);
    // Крупицы — не из воздуха: сколько выросло, столько и взято из своей
    // клетки, но не больше, чем в ней есть (как у травы).
    plant.minerals = std::min(soil.minerals, kPlantMinerals * plant.growth / kFull);
    soil.minerals -= plant.minerals;

    const auto entity = world.registry().create();
    world.registry().emplace<PlantComponent>(entity, plant);
    world.registry().emplace<PlantGenomeComponent>(entity, genome);
    world.registry().emplace<BushComponent>(entity);
    // Ягод на нём пока нет: они завяжутся сами, сроком (core/Berries.hpp).
    // Выдать их при генерации значило бы накормить первое поголовье из
    // ниоткуда — вещество в этом мире всегда откуда-то берётся.
    world.registry().emplace<BerryComponent>(entity);
    world.place(entity, x, y);
    return true;
}

} // namespace

void seedBushes(World& world, const PlantParams& params, unsigned seed) {
    const int width = world.area().width();
    const int height = world.area().height();
    const auto cellCount = static_cast<long long>(width) * height;
    if (cellCount == 0) {
        return;
    }

    auto species = makeBushSpecies(params.bushSpecies, static_cast<std::uint64_t>(seed));
    auto& speciesComponent = world.registry().get<PlantSpeciesComponent>(world.worldEntity());
    speciesComponent.bushes = species;
    if (species.empty()) {
        return;
    }

    const int target = static_cast<int>(static_cast<long long>(params.bushCoverage) * cellCount / kFull);
    if (target <= 0) {
        return;
    }

    // Расклад бюджета черт дробный — это генерация, а не состояние мира
    // (core/Scale.hpp), поэтому целая настройка переводится в долю здесь.
    const float mutationRate = static_cast<float>(params.mutationRate) / kFull;

    std::uint64_t state = mixSeed(seed, 0xB05CA5EEDBEEF001ull);

    // Купы раздаются видам по кругу, а не по одной на вид: ягодников на
    // карте должно быть несколько, и стоять они должны в разных местах —
    // иначе весь вид оказывается в одном углу и погибает вместе с ним.
    const int patches = std::max(1, target / kBushPatchSize);
    for (int patch = 0; patch < patches; ++patch) {
        const auto& archetype = species[static_cast<std::size_t>(patch) % species.size()];

        // Годность клетки под ЦЕНТР купы строже, чем под соседний куст:
        // нужна земля с крупицами, иначе ягоды не завяжутся вовсе, и ягодник
        // выйдет ягодником только по имени.
        const auto suitableCenter = [&](int x, int y) {
            if (world.area().isBlocked(x, y) || hasPlant(world, x, y)) {
                return false;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            if (terrain == entt::null || world.registry().all_of<WaterComponent>(terrain)) {
                return false;
            }
            return world.registry().get<const SoilComponent>(terrain).minerals >= kBushCenterMinerals;
        };
        seedNest(
            world.area(), kBushPatchSize, kBushPatchRadius, suitableCenter,
            [&](int x, int y) { return plantBush(world, x, y, archetype, mutationRate, params.bushLifespan, state); },
            state);
    }
}

// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendBushSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Bush seeding";
    out.push_back({g, "kBushPatchSize", static_cast<float>(kBushPatchSize)});
    out.push_back({g, "kBushPatchRadius", static_cast<float>(kBushPatchRadius)});
    out.push_back({g, "kBushCenterMinerals", static_cast<float>(kBushCenterMinerals)});
    out.push_back({g, "kSeedingMinSupply", static_cast<float>(kSeedingMinSupply)});
}

} // namespace goblins

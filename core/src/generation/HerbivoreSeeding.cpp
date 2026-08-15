#include "core/generation/HerbivoreSeeding.hpp"

#include <algorithm>
#include <cmath>

#include "core/components/DesireComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/HerbivoreGenomeComponent.hpp"
#include "core/components/HerbivoreSpeciesComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/HerbivoreGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// С каким запасом животное появляется на свет при генерации — доля от
// собственной ёмкости. Не полный (мир не должен выглядеть подарком) и не
// пустой (иначе первые же тики выкосили бы стадо до того, как оно найдёт
// воду).
constexpr float kInitialReserveShare = 0.6f;

// Ограничение попыток, как в BoulderScatter и GrassSeeding: на карте, где
// почти всё — вода и камень, случайный поиск свободной клетки не должен
// уйти в бесконечный цикл.
constexpr int kAttemptMultiplier = 40;

// Терраформирующий Entity тайла (PositionComponent + SoilComponent) — тот,
// у кого и почва, и, если есть, вода.
entt::entity terrainEntityAt(World& world, int x, int y) {
    for (const auto entity : world.area().cellAt(x, y).entities) {
        if (world.registry().all_of<SoilComponent>(entity)) {
            return entity;
        }
    }
    return entt::null;
}

// Есть ли поблизости трава. Животное выпускается не куда попало, а туда,
// где ему есть что есть: иначе половина стада появлялась бы посреди голого
// камня и умирала от голода, не успев ничего сделать, — а это не событие
// мира, а просто плохая расстановка.
bool grassInSight(const World& world, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int nx = x + dx;
            const int ny = y + dy;
            if (!world.area().inBounds(nx, ny)) {
                continue;
            }
            for (const auto entity : world.area().cellAt(nx, ny).entities) {
                if (world.registry().all_of<PlantComponent>(entity)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

void seedHerbivores(World& world, const HerbivoreParams& params, unsigned seed) {
    const int width = world.area().width();
    const int height = world.area().height();
    if (width <= 0 || height <= 0) {
        return;
    }

    // --- Свойства мира: выбираются здесь один раз, дальше HerbivoreSystem
    // их только читает (06_GameLoop.md, п.1a). animalRandomSeed нужен
    // потому, что система не имеет права хранить собственный генератор
    // между тиками (05_Entity.md, п.3).
    auto& worldProperties = world.registry().get<WorldPropertiesComponent>(world.worldEntity());
    worldProperties.animalMutationRate = params.mutationRate;
    worldProperties.animalRandomSeed = seed;

    // --- Виды: архетипы на World Entity (см. HerbivoreSpeciesComponent) ---
    auto species = makeHerbivoreSpecies(params.species, static_cast<std::uint64_t>(seed));
    auto& speciesComponent = world.registry().get<HerbivoreSpeciesComponent>(world.worldEntity());
    speciesComponent.archetypes = species;
    if (species.empty() || params.count <= 0) {
        return;
    }

    std::uint64_t state = mixSeed(seed, 0xA71ADA5C0FFEE001ull);

    int placed = 0;
    int attempts = 0;
    const int maxAttempts = params.count * kAttemptMultiplier;
    while (placed < params.count && attempts < maxAttempts) {
        ++attempts;

        const int x = static_cast<int>(randomUnit(state) * static_cast<float>(width)) % width;
        const int y = static_cast<int>(randomUnit(state) * static_cast<float>(height)) % height;

        // Непроходимый Entity занимает тайл полностью (04_WorldModel.md,
        // п.4). А вот другое травоядное и трава клетку не занимают:
        // животных на одной клетке может быть сколько угодно, и трава под
        // ними продолжает расти.
        if (world.area().isBlocked(x, y)) {
            continue;
        }
        const auto terrain = terrainEntityAt(world, x, y);
        if (terrain == entt::null) {
            continue;
        }
        const auto* water = world.registry().try_get<const WaterComponent>(terrain);
        if (water != nullptr && water->depth > kWadeDepth) {
            continue; // в реку не ставим — животное туда и само не пойдёт
        }

        const std::size_t speciesIndex =
            static_cast<std::size_t>(randomUnit(state) * static_cast<float>(species.size())) % species.size();
        const auto& archetype = species[speciesIndex];

        const HerbivoreGenomeComponent genome = mutateHerbivoreGenome(
            archetype, archetype, params.mutationRate, mixSeed(state, static_cast<std::uint64_t>(placed)));

        if (!grassInSight(world, x, y, static_cast<int>(std::lround(genome.perception)))) {
            continue;
        }

        HerbivoreComponent animal;
        // Стартовое стадо не должно быть строем ровесников: возраст
        // случайный в пределах до первой половины жизни, размер — тот, до
        // которого животное успело бы дорасти к этому возрасту. Иначе всё
        // поголовье взрослело, размножалось и умирало синхронными волнами.
        animal.age = randomUnit(state) * genome.maxAge * 0.5f;
        animal.sex = randomUnit(state) < 0.5f ? Sex::Female : Sex::Male;
        animal.energy = genome.energyCapacity * kInitialReserveShare;
        animal.water = genome.waterCapacity * kInitialReserveShare;

        const float grownTo =
            genome.maturityAge > 0.0f ? std::clamp(animal.age / genome.maturityAge, 0.0f, 1.0f) : 1.0f;
        const int wanted = static_cast<int>(std::ceil(grownTo * genome.proteinNeed));
        // Белок первого поголовья — не из воздуха: ровно столько, сколько
        // есть в клетке, и ровно столько же вернётся в мир перегноем.
        auto& soil = world.registry().get<SoilComponent>(terrain);
        animal.protein = std::min(soil.minerals, std::max(0, wanted));
        soil.minerals -= animal.protein;
        animal.growth = genome.proteinNeed > 0.0f
                             ? std::min(grownTo, static_cast<float>(animal.protein) / genome.proteinNeed)
                             : grownTo;

        // Желания в момент рождения мира — те, что следуют из тела: сытое
        // и напоенное животное ничего не хочет, и первый же тик пересчитает
        // их заново (HerbivoreSystem). Единственное, что действительно
        // задаётся здесь, — разброс готовности к размножению: иначе всё
        // стадо потянулось бы искать пару в один и тот же тик.
        DesireComponent desire;
        desire.mating = randomUnit(state);

        const auto entity = world.registry().create();
        world.registry().emplace<IdentityComponent>(
            entity, IdentityComponent{mixSeed(state, static_cast<std::uint64_t>(placed) + 1ull)});
        world.registry().emplace<HerbivoreComponent>(entity, animal);
        world.registry().emplace<HerbivoreGenomeComponent>(entity, genome);
        world.registry().emplace<DesireComponent>(entity, desire);
        world.place(entity, x, y);

        ++placed;
    }
}


// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendHerbivoreSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Herbivore seeding";
    out.push_back({g, "kWadeDepth", kWadeDepth});
    out.push_back({g, "kInitialReserveShare", kInitialReserveShare});
    out.push_back({g, "kAttemptMultiplier", static_cast<float>(kAttemptMultiplier)});
}

} // namespace goblins

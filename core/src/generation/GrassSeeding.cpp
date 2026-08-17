#include "core/generation/GrassSeeding.hpp"

#include <algorithm>
#include <cmath>

#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/Scale.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// Суше этого семя не сажаем даже при генерации: клетка виду не подходит,
// растение всё равно погибло бы от стресса за сотню тиков (PlantSystem).
// Тот же порог использует и расселение — см. kSeedMinSupply там.
constexpr int kSeedingMinSupply = 500;

// Ограничение попыток, как в BoulderScatter: на карте, где почти всё —
// вода/камень/чужая для всех видов почва, случайный поиск свободной
// клетки не должен уйти в бесконечный цикл.
constexpr int kAttemptMultiplier = 20;

// Терраформирующий Entity тайла (PositionComponent + SoilComponent, см.
// TerrainGenerator) — тот, у кого и почва, и, если есть, вода.
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

} // namespace

void seedGrass(World& world, const PlantParams& params, unsigned seed) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    // --- Свойства мира: выбираются здесь один раз, дальше PlantSystem их
    // только читает (06_GameLoop.md, п.1a). plantRandomSeed нужен потому,
    // что система не имеет права хранить собственный генератор между
    // тиками (05_Entity.md, п.3) и каждый раз собирает случайность из
    // seed'а мира, номера тика и координат клетки.
    auto& worldProperties = world.registry().get<WorldPropertiesComponent>(world.worldEntity());
    worldProperties.plantMutationRate = params.mutationRate;
    // Расклад бюджета черт дробный — это генерация, а не состояние мира
    // (core/Scale.hpp), поэтому целая настройка переводится в долю здесь.
    const float mutationRate = static_cast<float>(params.mutationRate) / kFull;
    worldProperties.humusDecayPeriod = params.humusDecayPeriod;
    worldProperties.plantRandomSeed = seed;

    // --- Виды: архетипы на World Entity (см. PlantSpeciesComponent) ---
    auto species = makeGrassSpecies(params.grassSpecies, static_cast<std::uint64_t>(seed));
    auto& speciesComponent = world.registry().get<PlantSpeciesComponent>(world.worldEntity());
    speciesComponent.archetypes = species;
    if (species.empty()) {
        return;
    }

    // --- Первые семена ---
    const int target = static_cast<int>(static_cast<long long>(params.grassCoverage) * cellCount / kFull);
    if (target <= 0) {
        return;
    }
    const int maxAttempts = target * kAttemptMultiplier;

    std::uint64_t state = mixSeed(seed, 0x51EEDB0A12D5EEDCull);
    int planted = 0;
    int attempts = 0;
    while (planted < target && attempts < maxAttempts) {
        ++attempts;

        const int x = static_cast<int>(randomUnit(state) * static_cast<float>(width)) % width;
        const int y = static_cast<int>(randomUnit(state) * static_cast<float>(height)) % height;

        // Трава не делит тайл с непроходимым объектом (04_WorldModel.md,
        // п.4), и на одной клетке не может быть двух растений.
        if (world.area().isBlocked(x, y) || hasPlant(world, x, y)) {
            continue;
        }
        const auto terrain = terrainEntityAt(world, x, y);
        if (terrain == entt::null) {
            continue;
        }
        // Слишком глубокая вода — но "слишком" у каждого генома своё
        // (water_tolerance), поэтому саму глубину берём здесь, а сравнение
        // делаем ниже, когда геном семени уже выбран. Тот же закон, что и
        // у PlantSystem: тонет не тот, под кем есть вода, а тот, под кем
        // её больше, чем он переносит.
        const auto* water = world.registry().try_get<const WaterComponent>(terrain);
        const int waterDepth = water != nullptr ? water->depth : 0;

        const std::size_t speciesIndex =
            static_cast<std::size_t>(randomUnit(state) * static_cast<float>(species.size())) % species.size();
        const auto& archetype = species[speciesIndex];

        // Каждое семя получает собственный геном: архетип вида плюс та же
        // мутация, что действует и при размножении, — генетическая
        // вариативность внутри вида существует с первого тика, а не
        // появляется через поколения.
        const PlantGenomeComponent genome =
            mutateGenome(archetype, archetype, mutationRate, mixSeed(state, static_cast<std::uint64_t>(planted)));

        if (waterDepth > genome.waterTolerance) {
            continue; // этот вид тут утонет
        }

        auto& soil = world.registry().get<SoilComponent>(terrain);
        // Пригодность клетки — та же обеспеченность влагой, по которой
        // растение живёт каждый тик (PlantSystem). Отдельной функции
        // пригодности почвы по каменистости и утоптанности больше нет:
        // камень и так суше, и виду, которому нужна вода, на нём нечего
        // делать по той же причине, что и в сухой степи.
        const int supply =
            genome.moistureNeed > 0 ? std::min(kFull, soil.moisture * kFull / genome.moistureNeed) : kFull;
        if (supply < kSeedingMinSupply) {
            continue; // вид сюда не годится — пусть его семена лягут там, где ему подходит
        }

        // Стартовый мир не должен быть строем ровесников: возраст
        // случайный в пределах до созревания, а размер — тот, до которого
        // растение успело бы дорасти к этому возрасту. Иначе весь луг
        // созревал бы и умирал синхронными волнами.
        PlantComponent plant;
        plant.age = static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(std::max(1, genome.maturityAge))));

        const int grownTo = std::min(kFull, plant.age * genome.growthRate);
        const int wanted = (grownTo * genome.mineralNeed + kFull - 1) / kFull;
        // Минералы стартовых растений — не из воздуха: ровно столько,
        // сколько есть в клетке, и ровно столько же вернётся в неё
        // перегноем после смерти.
        plant.minerals = std::min(soil.minerals, std::max(0, wanted));
        soil.minerals -= plant.minerals;
        plant.growth = genome.mineralNeed > 0
                            ? std::min(grownTo, plant.minerals * kFull / genome.mineralNeed)
                            : grownTo;

        const auto entity = world.registry().create();
        world.registry().emplace<PlantComponent>(entity, plant);
        world.registry().emplace<PlantGenomeComponent>(entity, genome);
        world.place(entity, x, y);

        ++planted;
    }
}


// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendGrassSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Grass seeding";
    out.push_back({g, "kSeedingMinSupply", static_cast<float>(kSeedingMinSupply)});
    out.push_back({g, "kAttemptMultiplier", static_cast<float>(kAttemptMultiplier)});
}

} // namespace goblins

#include "core/generation/AnimalSeeding.hpp"

#include <algorithm>
#include <cmath>
#include <span>

#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// С каким запасом животное появляется на свет при генерации — доля от
// собственной ёмкости. Не полный (мир не должен выглядеть подарком) и не
// пустой (иначе первые же тики выкосили бы поголовье до того, как оно
// найдёт воду).
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

// Есть ли в пределах радиуса то, чем эта диета кормится. Животное
// выпускается не куда попало, а туда, где ему есть что есть: иначе
// половина поголовья появлялась бы посреди голого камня и умирала от
// голода, не успев ничего сделать, — а это не событие мира, а просто
// плохая расстановка.
template <typename Food>
bool foodInSight(const World& world, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int nx = x + dx;
            const int ny = y + dy;
            if (!world.area().inBounds(nx, ny)) {
                continue;
            }
            for (const auto entity : world.area().cellAt(nx, ny).entities) {
                if (world.registry().all_of<Food>(entity)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Выпустить одно поголовье: count особей вида из species, каждой — свой
// геном, пол, возраст и запас. Diet — тег диеты (HerbivoreComponent или
// PredatorComponent), Food — то, наличие чего рядом делает клетку годной
// для выпуска (PlantComponent для травоядных, HerbivoreComponent для
// хищников).
//
// Один код на оба поголовья: различаются они только этими двумя типами и
// таблицей черт, а всё остальное — расстановка, возраст, запасы, вычет
// белка из почвы — у них одинаково.
template <typename Diet, typename Food>
void releaseAnimals(World& world, const std::vector<AnimalGenomeComponent>& species,
                    std::span<const AnimalTrait> traits, int count, float mutationRate, std::uint64_t& state) {
    if (species.empty() || count <= 0) {
        return;
    }

    const int width = world.area().width();
    const int height = world.area().height();

    int placed = 0;
    int attempts = 0;
    const int maxAttempts = count * kAttemptMultiplier;
    while (placed < count && attempts < maxAttempts) {
        ++attempts;

        const int x = static_cast<int>(randomUnit(state) * static_cast<float>(width)) % width;
        const int y = static_cast<int>(randomUnit(state) * static_cast<float>(height)) % height;

        // Непроходимый Entity занимает тайл полностью (04_WorldModel.md,
        // п.4). А вот другое животное и трава клетку не занимают: животных
        // на одной клетке может быть сколько угодно, и трава под ними
        // продолжает расти.
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

        const AnimalGenomeComponent genome = mutateGenome(
            traits, archetype, archetype, mutationRate, mixSeed(state, static_cast<std::uint64_t>(placed)));

        if (!foodInSight<Food>(world, x, y, static_cast<int>(std::lround(genome.perception)))) {
            continue;
        }

        AnimalComponent animal;
        // Стартовое поголовье не должно быть строем ровесников: возраст
        // случайный в пределах до первой половины жизни, размер — тот, до
        // которого животное успело бы дорасти к этому возрасту. Иначе оно
        // взрослело, размножалось и умирало синхронными волнами.
        animal.age = randomUnit(state) * genome.maxAge * 0.5f;
        animal.sex = randomUnit(state) < 0.5f ? Sex::Female : Sex::Male;
        animal.energy = genome.energyCapacity * kInitialReserveShare;
        animal.water = genome.waterCapacity * kInitialReserveShare;

        const float grownTo =
            genome.maturityAge > 0.0f ? std::clamp(animal.age / genome.maturityAge, 0.0f, 1.0f) : 1.0f;
        const int wanted = static_cast<int>(std::ceil(grownTo * genome.proteinNeed));
        // Белок первого поголовья — не из воздуха: ровно столько, сколько
        // есть в клетке, и ровно столько же вернётся в мир падалью.
        //
        // Если клетка бедна и тела не хватает, ищем другую, а не выпускаем
        // недоросля: животное, родившееся с третью нужного белка, так и
        // остаётся мелким, не может ни охотиться, ни принести потомство, и
        // всю жизнь доедает чужое. Это не событие мира, а неудачная
        // расстановка.
        auto& soil = world.registry().get<SoilComponent>(terrain);
        if (soil.minerals < wanted) {
            continue;
        }
        animal.protein = std::max(0, wanted);
        soil.minerals -= animal.protein;
        animal.growth = genome.proteinNeed > 0.0f
                             ? std::min(grownTo, static_cast<float>(animal.protein) / genome.proteinNeed)
                             : grownTo;

        // Желания в момент рождения мира — те, что следуют из тела: сытое
        // и напоенное животное ничего не хочет, и первый же тик пересчитает
        // их заново (AnimalSystem). Единственное, что действительно
        // задаётся здесь, — разброс готовности к размножению: иначе всё
        // поголовье потянулось бы искать пару в один и тот же тик.
        DesireComponent desire;
        desire.mating = randomUnit(state);

        const auto entity = world.registry().create();
        world.registry().emplace<IdentityComponent>(
            entity, IdentityComponent{mixSeed(state, static_cast<std::uint64_t>(placed) + 1ull)});
        world.registry().emplace<AnimalComponent>(entity, animal);
        world.registry().emplace<AnimalGenomeComponent>(entity, genome);
        world.registry().emplace<DesireComponent>(entity, desire);
        world.registry().emplace<Diet>(entity);
        world.place(entity, x, y);

        ++placed;
    }
}

} // namespace

void seedAnimals(World& world, const AnimalParams& params, unsigned seed) {
    if (world.area().width() <= 0 || world.area().height() <= 0) {
        return;
    }

    // --- Свойства мира: выбираются здесь один раз, дальше AnimalSystem их
    // только читает (06_GameLoop.md, п.1a). animalRandomSeed нужен потому,
    // что система не имеет права хранить собственный генератор между
    // тиками (05_Entity.md, п.3).
    auto& worldProperties = world.registry().get<WorldPropertiesComponent>(world.worldEntity());
    worldProperties.animalMutationRate = params.mutationRate;
    worldProperties.animalRandomSeed = seed;

    // --- Виды: архетипы на World Entity (см. AnimalSpeciesComponent) ---
    auto& speciesComponent = world.registry().get<AnimalSpeciesComponent>(world.worldEntity());
    speciesComponent.herbivores = makeHerbivoreSpecies(params.herbivoreSpecies, static_cast<std::uint64_t>(seed));
    speciesComponent.predators = makePredatorSpecies(params.predatorSpecies, static_cast<std::uint64_t>(seed));

    std::uint64_t state = mixSeed(seed, 0xA71ADA5C0FFEE001ull);

    // Травоядные первыми: хищник выпускается туда, где ему уже видно, на
    // кого охотиться, — значит добыча к этому моменту должна стоять на
    // карте. Та же причина, по которой сам этот этап идёт после травы.
    releaseAnimals<HerbivoreComponent, PlantComponent>(world, speciesComponent.herbivores, herbivoreTraits(),
                                                        params.herbivoreCount, params.mutationRate, state);
    releaseAnimals<PredatorComponent, HerbivoreComponent>(world, speciesComponent.predators, predatorTraits(),
                                                           params.predatorCount, params.mutationRate, state);
}


// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendAnimalSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Animal seeding";
    out.push_back({g, "kWadeDepth", kWadeDepth});
    out.push_back({g, "kInitialReserveShare", kInitialReserveShare});
    out.push_back({g, "kAttemptMultiplier", static_cast<float>(kAttemptMultiplier)});
}

} // namespace goblins

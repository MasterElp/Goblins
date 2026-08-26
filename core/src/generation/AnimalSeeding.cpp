#include "core/generation/AnimalSeeding.hpp"

#include <algorithm>
#include <cmath>
#include <span>

#include "core/Scale.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/InjuryComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/Nest.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// С каким запасом животное появляется на свет при генерации — доля от
// собственной ёмкости. Не полный (мир не должен выглядеть подарком) и не
// пустой (иначе первые же тики выкосили бы поголовье до того, как оно
// найдёт воду).
constexpr int kInitialReserveShare = 600;

// Докуда стадо расходится от своего центра. Широко: стадо кормится на ходу
// и за первую же сотню тиков разбредается само, поэтому число здесь — не
// размер стада, а лишь предел, за который расстановка не выходит. Теснее
// племени (kTribeRadius в GoblinSeeding) впятеро с лишним, и это верно по
// сути: гоблины живут местом, а стадо — пастбищем.
//
// Перебора попыток рядом больше нет: поиск клетки теперь внутри гнезда
// (core/generation/Nest.hpp), и ограничен он там.
constexpr int kHerdRadius = 20;

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

    // Каждый вид выпускается СВОИМ стадом, а не рассыпается по карте
    // поодиночке. Рассыпанное животное живёт в мире, где его сородичи —
    // редкая случайность: пару ему искать некого (core/Mating.hpp видит
    // только то, что рядом), стада как явления не возникает вовсе, а вид
    // держится лишь на том, что кто-то случайно набрёл на кого-то.
    //
    // Раскладка — общий закон гнезда (core/generation/Nest.hpp), тот же, что
    // у рощи, ягодника и племени. Здесь остаётся своё: чем годен центр (еда
    // этой диеты в пределах видимости) и как выпускается одна особь.
    const int perSpecies = std::max(1, count / static_cast<int>(species.size()));
    int born = 0;
    for (const auto& archetype : species) {
        // Годность клетки под ЦЕНТР стада строже, чем под соседнюю особь:
        // от центра зависит, где вид живёт всю первую сотню тиков, поэтому
        // еда рядом с ним обязательна.
        const auto suitableCenter = [&](int x, int y) {
            if (world.area().isBlocked(x, y)) {
                return false;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            if (terrain == entt::null || world.registry().all_of<WaterComponent>(terrain)) {
                return false;
            }
            return foodInSight<Food>(world, x, y, std::max(1, archetype.perception));
        };

        const auto release = [&](int x, int y) {
            // Непроходимый Entity занимает тайл полностью (04_WorldModel.md,
            // п.4). А вот другое животное и трава клетку не занимают: животных
            // на одной клетке может быть сколько угодно, и трава под ними
            // продолжает расти.
            if (world.area().isBlocked(x, y)) {
                return false;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            if (terrain == entt::null) {
                return false;
            }
            // В воду не ставим — животное туда и само не пойдёт: вода для него
            // такая же стена, как булыжник (AnimalSystem.cpp, standable).
            // Разойдись эти две проверки, и стадо оказалось бы расставленным
            // там, откуда оно не может сойти.
            if (world.registry().all_of<WaterComponent>(terrain)) {
                return false;
            }

            const AnimalGenomeComponent genome = mutateGenome(
                traits, archetype, archetype, mutationRate, mixSeed(state, static_cast<std::uint64_t>(born)));

            if (!foodInSight<Food>(world, x, y, genome.perception)) {
                return false;
            }

            AnimalComponent animal;
            // Стартовое поголовье не должно быть строем ровесников: возраст
            // случайный в пределах до первой половины жизни, размер — тот, до
            // которого животное успело бы дорасти к этому возрасту. Иначе оно
            // взрослело, размножалось и умирало синхронными волнами.
            animal.age =
                static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(std::max(1, genome.maxAge / 2))));
            animal.sex = randomBelow(state, 2) == 0 ? Sex::Female : Sex::Male;
            animal.energy = genome.energyCapacity * kInitialReserveShare / kFull;
            animal.water = genome.waterCapacity * kInitialReserveShare / kFull;

            const int grownTo =
                genome.maturityAge > 0 ? std::min(kFull, animal.age * kFull / genome.maturityAge) : kFull;
            const int wanted = (grownTo * genome.proteinNeed + kFull - 1) / kFull;
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
                return false;
            }
            animal.protein = std::max(0, wanted);
            soil.minerals -= animal.protein;
            animal.growth = genome.proteinNeed > 0
                                 ? std::min(grownTo, animal.protein * kFull / genome.proteinNeed)
                                 : grownTo;

            // Желания в момент рождения мира — те, что следуют из тела: сытое
            // и напоенное животное ничего не хочет, и первый же тик пересчитает
            // их заново (AnimalSystem). Единственное, что действительно
            // задаётся здесь, — разброс готовности к размножению: иначе всё
            // поголовье потянулось бы искать пару в один и тот же тик.
            DesireComponent desire;
            desire.mating = static_cast<int>(randomBelow(state, kFull));

            const auto entity = world.registry().create();
            world.registry().emplace<IdentityComponent>(
                entity, IdentityComponent{mixSeed(state, static_cast<std::uint64_t>(born) + 1ull)});
            world.registry().emplace<AnimalComponent>(entity, animal);
            world.registry().emplace<AnimalGenomeComponent>(entity, genome);
            world.registry().emplace<DesireComponent>(entity, desire);
            // Память ног пуста: расставленное животное ещё никуда не ходило.
            world.registry().emplace<MovementComponent>(entity);
            // И увечий у него ещё нет: первое поголовье выходит в мир целым.
            world.registry().emplace<InjuryComponent>(entity);
            world.registry().emplace<Diet>(entity);
            world.place(entity, x, y);

            ++born;
            return true;
        };

        seedNest(world.area(), perSpecies, kHerdRadius, suitableCenter, release, state);
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
    // Как и у травы: целая настройка мира — дробная доля для раскладов
    // бюджета (core/Scale.hpp).
    const float mutationRate = static_cast<float>(params.mutationRate) / kFull;
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
                                                        params.herbivoreCount, mutationRate, state);
    releaseAnimals<PredatorComponent, HerbivoreComponent>(world, speciesComponent.predators, predatorTraits(),
                                                           params.predatorCount, mutationRate, state);
}


// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendAnimalSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Animal seeding";
    out.push_back({g, "kInitialReserveShare", static_cast<float>(kInitialReserveShare)});
    out.push_back({g, "kHerdRadius", static_cast<float>(kHerdRadius)});
}

} // namespace goblins

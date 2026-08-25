#include "core/generation/GoblinSeeding.hpp"

#include <algorithm>
#include <span>

#include "core/Scale.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/GoblinDesireComponent.hpp"
#include "core/components/GoblinTribesComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/KnowledgeComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/GoblinGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// С каким запасом гоблин появляется на свет при генерации — доля от
// собственной ёмкости. То же число и по той же причине, что у животных
// (kInitialReserveShare в AnimalSeeding): не полный (мир не должен выглядеть
// подарком) и не пустой (иначе первые же тики выкосили бы поголовье до того,
// как оно найдёт воду).
constexpr int kInitialReserveShare = 600;

// Ограничение попыток, как в AnimalSeeding, BoulderScatter и GrassSeeding: на
// карте, где почти всё — вода и камень, случайный поиск свободной клетки не
// должен уйти в бесконечный цикл. Множитель больше звериного: годных клеток
// гоблину меньше — ему нужна трава в пределах видимости, а видит он не
// дальше самого зоркого травоядного, которое эту траву уже заняло.
constexpr int kAttemptMultiplier = 60;

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

// Растёт ли в пределах радиуса что-нибудь съедобное. Гоблин выпускается не
// куда попало, а туда, где ему есть что есть: иначе половина поголовья
// появлялась бы посреди голого камня и умирала от голода, не успев ничего
// сделать, — а это не событие мира, а плохая расстановка.
bool plantInSight(const World& world, int x, int y, int radius) {
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

void seedGoblins(World& world, const GoblinParams& params, unsigned seed) {
    if (world.area().width() <= 0 || world.area().height() <= 0) {
        return;
    }

    // --- Свойства мира: выбираются здесь один раз, дальше GoblinSystem их
    // только читает (06_GameLoop.md, п.1a). goblinRandomSeed нужен потому,
    // что система не имеет права хранить собственный генератор между тиками
    // (05_Entity.md, п.3).
    auto& worldProperties = world.registry().get<WorldPropertiesComponent>(world.worldEntity());
    worldProperties.goblinMutationRate = params.mutationRate;
    worldProperties.goblinRandomSeed = seed;
    // Целая настройка мира — дробная доля для раскладов бюджета
    // (core/Scale.hpp): расклад дробен, потому что он генерация, а не
    // состояние мира.
    const float mutationRate = static_cast<float>(params.mutationRate) / kFull;

    // --- Племена: архетипы на World Entity (см. GoblinTribesComponent) ---
    auto& tribesComponent = world.registry().get<GoblinTribesComponent>(world.worldEntity());
    tribesComponent.tribes = makeGoblinTribes(params.tribes, static_cast<std::uint64_t>(seed));
    if (tribesComponent.tribes.empty() || params.count <= 0) {
        return;
    }

    std::uint64_t state = mixSeed(seed, 0x60B11D5EED0FF1CEull);

    const int width = world.area().width();
    const int height = world.area().height();
    const auto traits = goblinTraits();

    int placed = 0;
    int attempts = 0;
    const int maxAttempts = params.count * kAttemptMultiplier;
    while (placed < params.count && attempts < maxAttempts) {
        ++attempts;

        const int x = static_cast<int>(randomUnit(state) * static_cast<float>(width)) % width;
        const int y = static_cast<int>(randomUnit(state) * static_cast<float>(height)) % height;

        // Непроходимый Entity занимает тайл полностью (04_WorldModel.md,
        // п.4). А вот другое существо и трава клетку не занимают.
        if (world.area().isBlocked(x, y)) {
            continue;
        }
        const auto terrain = terrainEntityAt(world, x, y);
        if (terrain == entt::null) {
            continue;
        }
        // В воду не ставим — гоблин туда и сам не пойдёт: вода для него
        // такая же стена, как булыжник (core/Path.hpp, standableAt).
        if (world.registry().all_of<WaterComponent>(terrain)) {
            continue;
        }

        const std::size_t tribeIndex =
            static_cast<std::size_t>(randomUnit(state) * static_cast<float>(tribesComponent.tribes.size())) %
            tribesComponent.tribes.size();
        const auto& archetype = tribesComponent.tribes[tribeIndex];

        const AnimalGenomeComponent genome = mutateGenome(
            traits, archetype, archetype, mutationRate, mixSeed(state, static_cast<std::uint64_t>(placed)));

        if (!plantInSight(world, x, y, genome.perception)) {
            continue;
        }

        AnimalComponent body;
        // Первое поголовье не должно быть строем ровесников: возраст
        // случайный в пределах до первой половины жизни, размер — тот, до
        // которого гоблин успел бы дорасти к этому возрасту. Иначе они
        // взрослели, сходились и умирали синхронными волнами.
        body.age = static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(std::max(1, genome.maxAge / 2))));
        body.sex = randomBelow(state, 2) == 0 ? Sex::Female : Sex::Male;
        body.energy = genome.energyCapacity * kInitialReserveShare / kFull;
        body.water = genome.waterCapacity * kInitialReserveShare / kFull;

        const int grownTo = genome.maturityAge > 0 ? std::min(kFull, body.age * kFull / genome.maturityAge) : kFull;
        const int wanted = (grownTo * genome.proteinNeed + kFull - 1) / kFull;
        // Белок первого поголовья — не из воздуха: ровно столько, сколько
        // есть в клетке, и ровно столько же вернётся в мир падалью. Если
        // клетка бедна, ищем другую, а не выпускаем недоросля.
        auto& soil = world.registry().get<SoilComponent>(terrain);
        if (soil.minerals < wanted) {
            continue;
        }
        body.protein = std::max(0, wanted);
        soil.minerals -= body.protein;
        body.growth = genome.proteinNeed > 0 ? std::min(grownTo, body.protein * kFull / genome.proteinNeed)
                                              : grownTo;

        // Желания в момент рождения мира — те, что следуют из тела, и первый
        // же тик пересчитает их заново (GoblinSystem). Единственное, что
        // действительно задаётся здесь, — разброс готовности к продолжению
        // рода: иначе всё поголовье потянулось бы искать пару в один тик.
        GoblinDesireComponent desire;
        desire.mating = static_cast<int>(randomBelow(state, kFull));

        const auto entity = world.registry().create();
        world.registry().emplace<IdentityComponent>(
            entity, IdentityComponent{mixSeed(state, static_cast<std::uint64_t>(placed) + 1ull)});
        world.registry().emplace<AnimalComponent>(entity, body);
        world.registry().emplace<AnimalGenomeComponent>(entity, genome);
        world.registry().emplace<GoblinDesireComponent>(entity, desire);
        // Память ног пуста: расставленный гоблин ещё никуда не ходил.
        world.registry().emplace<MovementComponent>(entity);
        world.registry().emplace<GoblinComponent>(entity);
        // Память пуста: расставленный гоблин ещё нигде не был. Мир он узнает
        // сам, блужданием, — и тем, что запомнит, будет отличаться от соседа.
        world.registry().emplace<KnowledgeComponent>(entity);
        world.place(entity, x, y);

        ++placed;
    }
}

// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendGoblinSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Goblin seeding";
    out.push_back({g, "kInitialReserveShare", static_cast<float>(kInitialReserveShare)});
    out.push_back({g, "kAttemptMultiplier", static_cast<float>(kAttemptMultiplier)});
}

} // namespace goblins

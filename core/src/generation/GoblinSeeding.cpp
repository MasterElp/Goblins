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
#include "core/components/BushComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/GoblinGenetics.hpp"
#include "core/generation/Nest.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

// С каким запасом гоблин появляется на свет при генерации — доля от
// собственной ёмкости. То же число и по той же причине, что у животных
// (kInitialReserveShare в AnimalSeeding): не полный (мир не должен выглядеть
// подарком) и не пустой (иначе первые же тики выкосили бы поголовье до того,
// как оно найдёт воду).
constexpr int kInitialReserveShare = 600;

// Докуда расходится от своего центра ПЛЕМЯ. Тесно, и это главное число всей
// расстановки: два десятка гоблинов в круге радиусом восемь — это соседи,
// которые видят друг друга и ходят по одной округе, а не случайные
// встречные. Именно из соседства и может вырасти поселение; из россыпи по
// карте — не может ничего.
//
// 02_CorePrinciples.md, п.16 говорит про «около трёх поселений примерно по
// двадцать жителей». До этого шага правило нарушалось на первом же тике:
// каждый гоблин бросался в случайную клетку карты, а племя разыгрывалось
// для каждого отдельно — племена были размазаны и перемешаны.
//
// Втрое теснее стада (kHerdRadius в AnimalSeeding), и это верно по сути:
// стадо живёт пастбищем, а племя — местом.
constexpr int kTribeRadius = 8;

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

// Есть ли в пределах радиуса ЯГОДНИК. Гоблин выпускается не куда попало, а
// туда, где ему есть что есть: иначе половина поголовья появлялась бы
// посреди голого камня и умирала от голода, не успев ничего сделать, — а это
// не событие мира, а плохая расстановка.
//
// Куст, а не любое растение: гоблин — собиратель, трава ему голодный запас,
// а не еда, за которой ходят (см. GoblinSystem). Расстановка "лишь бы рядом
// что-то росло" выпускала бы племя посреди луга, где ему нечего есть.
bool bushInSight(const World& world, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int nx = x + dx;
            const int ny = y + dy;
            if (!world.area().inBounds(nx, ny)) {
                continue;
            }
            for (const auto entity : world.area().cellAt(nx, ny).entities) {
                if (world.registry().all_of<PlantComponent, BushComponent>(entity)) {
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

    const auto traits = goblinTraits();

    // Каждое племя селится СВОИМ гнездом: у него один центр и одна округа.
    // Раскладка — общий закон (core/generation/Nest.hpp), тот же, что у рощи,
    // ягодника и стада; здесь остаётся своё — чем годен центр и как
    // выпускается один гоблин.
    const int perTribe = std::max(1, params.count / static_cast<int>(tribesComponent.tribes.size()));
    int placed = 0;
    for (std::size_t tribeIndex = 0; tribeIndex < tribesComponent.tribes.size(); ++tribeIndex) {
        const auto& archetype = tribesComponent.tribes[tribeIndex];

        // Центр племени выбирается придирчиво: рядом обязан быть ягодник.
        // От этой клетки зависит, где племя проживёт первые сотни тиков и
        // будет ли ему куда возвращаться (docs/10_Goblins.md, п.4b).
        const auto suitableCenter = [&](int x, int y) {
            if (world.area().isBlocked(x, y)) {
                return false;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            if (terrain == entt::null || world.registry().all_of<WaterComponent>(terrain)) {
                return false;
            }
            return bushInSight(world, x, y, std::max(1, archetype.perception));
        };

        const auto release = [&](int x, int y) {
            // Непроходимый Entity занимает тайл полностью (04_WorldModel.md,
            // п.4). А вот другое существо и трава клетку не занимают.
            if (world.area().isBlocked(x, y)) {
                return false;
            }
            const auto terrain = terrainEntityAt(world, x, y);
            if (terrain == entt::null) {
                return false;
            }
            // В воду не ставим — гоблин туда и сам не пойдёт: вода для него
            // такая же стена, как булыжник (core/Path.hpp, standableAt).
            if (world.registry().all_of<WaterComponent>(terrain)) {
                return false;
            }

            const AnimalGenomeComponent genome = mutateGenome(
                traits, archetype, archetype, mutationRate, mixSeed(state, static_cast<std::uint64_t>(placed)));

            // Ягодник в пределах видимости — уже не у центра, а у каждого:
            // край гнезда может уйти за холм, и выпускать туда голодного
            // незачем.
            if (!bushInSight(world, x, y, genome.perception)) {
                return false;
            }

            AnimalComponent body;
            // Первое поголовье не должно быть строем ровесников: возраст
            // случайный в пределах до первой половины жизни, размер — тот, до
            // которого гоблин успел бы дорасти к этому возрасту. Иначе они
            // взрослели, сходились и умирали синхронными волнами.
            body.age =
                static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(std::max(1, genome.maxAge / 2))));
            body.sex = randomBelow(state, 2) == 0 ? Sex::Female : Sex::Male;
            body.energy = genome.energyCapacity * kInitialReserveShare / kFull;
            body.water = genome.waterCapacity * kInitialReserveShare / kFull;

            const int grownTo =
                genome.maturityAge > 0 ? std::min(kFull, body.age * kFull / genome.maturityAge) : kFull;
            const int wanted = (grownTo * genome.proteinNeed + kFull - 1) / kFull;
            // Белок первого поголовья — не из воздуха: ровно столько, сколько
            // есть в клетке, и ровно столько же вернётся в мир падалью. Если
            // клетка бедна, ищем другую, а не выпускаем недоросля.
            auto& soil = world.registry().get<SoilComponent>(terrain);
            if (soil.minerals < wanted) {
                return false;
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
            return true;
        };

        seedNest(world.area(), perTribe, kTribeRadius, suitableCenter, release, state);
    }
}

// Константы этой стадии — наружу только для чтения (core/Diagnostics.hpp).
void appendGoblinSeedingConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Goblin seeding";
    out.push_back({g, "kInitialReserveShare", static_cast<float>(kInitialReserveShare)});
    out.push_back({g, "kTribeRadius", static_cast<float>(kTribeRadius)});
}

} // namespace goblins

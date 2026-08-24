#include "core/Simulation.hpp"

#include "core/generation/AnimalSeeding.hpp"
#include "core/generation/BoulderScatter.hpp"
#include "core/generation/GrassSeeding.hpp"
#include "core/generation/TreeSeeding.hpp"
#include "core/systems/AnimalSystem.hpp"
#include "core/systems/HydrologySystem.hpp"
#include "core/systems/PlantSystem.hpp"
#include "core/systems/TimeSystem.hpp"

namespace goblins {

namespace {

// Смещения seed'а по стадиям: см. WorldGenParams::seed. Держим их
// подальше от внутренних смещений generateTerrain (seed, seed+1, seed+2,
// seed+4, seed+10, seed+20), чтобы стадии не столкнулись на одном числе.
constexpr unsigned kBoulderSeedOffset = 1000;
constexpr unsigned kPlantSeedOffset = 2000;
constexpr unsigned kTreeSeedOffset = 2500;
constexpr unsigned kAnimalSeedOffset = 3000;

} // namespace

GenerationStats generateWorld(World& world, int width, int height, const WorldGenParams& params) {
    world.reset(width, height);

    // 1. Карта и природные ресурсы: рельеф, реки, пруды, почва, минералы.
    const auto stats = generateTerrain(world, params.seed, params.terrain);

    // 2. Непроходимые природные объекты — уже поверх готового рельефа: в
    //    воду булыжник не ложится.
    scatterBoulders(world, params.boulderCount, params.seed + kBoulderSeedOffset);

    // 3. Заселение растительностью: она должна видеть и водоёмы, и занятые
    //    тайлы, чтобы не сесть на воду и не занять чужой
    //    (02_CorePrinciples.md, п.5).
    //
    //    Деревья — первыми: кто занимает место раньше, тот его и получает, а
    //    луг зарастает всюду, где ему хватает влаги, и дереву после него не
    //    осталось бы ни одной свободной клетки (см. TreeSeeding.hpp).
    seedTrees(world, params.plants, params.seed + kTreeSeedOffset);
    seedGrass(world, params.plants, params.seed + kPlantSeedOffset);

    // 4. Появление животных — последним: травоядное выпускается туда, где
    //    ему есть что есть, а хищник — туда, где есть на кого охотиться,
    //    поэтому и трава, и стадо к этому моменту должны уже стоять на
    //    карте (02_CorePrinciples.md, п.5).
    seedAnimals(world, params.animals, params.seed + kAnimalSeedOffset);

    return stats;
}

void addDefaultSystems(GameLoop& loop) {
    // Первым — время: все остальные системы этого тика видят уже новый
    // номер (06_GameLoop.md, п.2).
    loop.addSystem(TimeSystem);
    // Затем вода и почва: течение, эрозия, испарение, дождь, влажность,
    // минералы.
    loop.addSystem(HydrologySystem);
    // Затем растения — по почве, уже приведённой водой в состояние этого
    // тика, а не прошлого.
    loop.addSystem(PlantSystem);
    // И последними — животные: они ходят по уже сложившемуся миру, едят
    // траву такой, какой она стала на этом тике, а то, что они с ней
    // сделали (объели, вытоптали), трава увидит на следующем — системы
    // разговаривают только состоянием компонентов (05_Entity.md, п.6).
    // Травоядные и хищники — одна система: животное у них одно, разная
    // только диета (см. AnimalSystem.hpp).
    loop.addSystem(AnimalSystem);
}

} // namespace goblins

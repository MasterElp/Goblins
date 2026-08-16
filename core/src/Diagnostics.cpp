#include "core/Diagnostics.hpp"

namespace goblins {

std::vector<ConstantInfo> coreConstants() {
    std::vector<ConstantInfo> all;
    // Порядок — как в игровом цикле и в генерации: сначала то, что делает
    // карту, потом то, что её каждый тик меняет.
    appendTerrainConstants(all);
    appendHydrologyConstants(all);
    appendGrassSeedingConstants(all);
    appendPlantGeneticsConstants(all);
    appendPlantSystemConstants(all);
    appendAnimalSeedingConstants(all);
    appendAnimalGeneticsConstants(all);
    appendAnimalSystemConstants(all);
    return all;
}

} // namespace goblins

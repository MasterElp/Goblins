#include "core/generation/HerbivoreGenetics.hpp"
#include "core/Diagnostics.hpp"

#include <algorithm>

namespace goblins {

namespace {

// Своя "соль" — не та, что у видов травы (kGrassSpeciesSalt): на одном и
// том же seed мира наборы видов травы и травоядных не должны быть одной и
// той же последовательностью.
constexpr std::uint64_t kHerbivoreSpeciesSalt = 0xBEA57C0DE5EED00Dull;

std::span<const HerbivoreTrait> traits() {
    return std::span<const HerbivoreTrait>(kHerbivoreTraits, kHerbivoreTraitCount);
}

} // namespace

std::vector<HerbivoreGenomeComponent> makeHerbivoreSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinHerbivoreSpecies, kMaxHerbivoreSpecies);
    return genetics::makeSpecies(traits(), count, seed, kHerbivoreSpeciesSalt);
}

HerbivoreGenomeComponent crossHerbivoreGenomes(const HerbivoreGenomeComponent& mother,
                                                const HerbivoreGenomeComponent& father,
                                                const HerbivoreGenomeComponent& archetype, float mutationRate,
                                                std::uint64_t seed) {
    return genetics::cross(traits(), mother, father, archetype, mutationRate, seed);
}

HerbivoreGenomeComponent mutateHerbivoreGenome(const HerbivoreGenomeComponent& parent,
                                                const HerbivoreGenomeComponent& archetype, float mutationRate,
                                                std::uint64_t seed) {
    return genetics::mutate(traits(), parent, archetype, mutationRate, seed);
}


// Константы этой таблицы — наружу только для чтения (core/Diagnostics.hpp).
// Общая механика бюджета перечислена в группе "Genetics"
// (appendPlantGeneticsConstants) — она одна на всё живое, дублировать её
// здесь незачем.
void appendHerbivoreGeneticsConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Herbivore genetics";
    out.push_back({g, "kMinHerbivoreSpecies", static_cast<float>(kMinHerbivoreSpecies)});
    out.push_back({g, "kMaxHerbivoreSpecies", static_cast<float>(kMaxHerbivoreSpecies)});
    out.push_back({g, "kHerbivoreTraitCount", static_cast<float>(kHerbivoreTraitCount)});
}

} // namespace goblins

#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

#include <algorithm>
#include <cmath>

namespace goblins {

namespace {

// Отдельная "соль" от той, что берёт мутация, и от той, что берут виды
// травоядных: наборы видов и мутации отдельного семечка не должны быть
// одной и той же последовательностью при одинаковом seed.
constexpr std::uint64_t kGrassSpeciesSalt = 0xD1CE5EEDA5510C11ull;

std::span<const PlantTrait> traits() {
    return std::span<const PlantTrait>(kGrassTraits, kGrassTraitCount);
}

} // namespace

std::vector<PlantGenomeComponent> makeGrassSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinGrassSpecies, kMaxGrassSpecies);
    return genetics::makeSpecies(traits(), count, seed, kGrassSpeciesSalt);
}

PlantGenomeComponent mutateGenome(const PlantGenomeComponent& parent, const PlantGenomeComponent& archetype,
                                  float mutationRate, std::uint64_t seed) {
    return genetics::mutate(traits(), parent, archetype, mutationRate, seed);
}

// Константы генетики — наружу только для чтения (core/Diagnostics.hpp).
// Общие для всего живого (бюджет, полоса вида, пороги различимости) живут
// в core/generation/Genetics.hpp и перечислены здесь же: в оверлее им место
// рядом, а не в отдельной группе на два числа.
void appendPlantGeneticsConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Genetics";
    out.push_back({g, "kMinAdvantage", genetics::kMinAdvantage});
    out.push_back({g, "kMaxAdvantage", genetics::kMaxAdvantage});
    out.push_back({g, "kBudgetFitIterations", static_cast<float>(genetics::kBudgetFitIterations)});
    out.push_back({g, "kMinSpeciesDistance", genetics::kMinSpeciesDistance});
    out.push_back({g, "kSpeciesAttempts", static_cast<float>(genetics::kSpeciesAttempts)});
    out.push_back({g, "kAdvantageBudgetShare", genetics::kAdvantageBudgetShare});
    out.push_back({g, "kSpeciesBand", genetics::kSpeciesBand});
    out.push_back({g, "kMinGrassSpecies", static_cast<float>(kMinGrassSpecies)});
    out.push_back({g, "kMaxGrassSpecies", static_cast<float>(kMaxGrassSpecies)});
}

} // namespace goblins

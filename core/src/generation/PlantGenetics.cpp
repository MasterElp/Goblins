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
// Своя соль у деревьев: при одном seed мира набор рощ не должен быть той же
// последовательностью, что набор трав.
constexpr std::uint64_t kTreeSpeciesSalt = 0x7BEE5EEDC0FFEE11ull;

std::span<const PlantTrait> traits() {
    return std::span<const PlantTrait>(kGrassTraits, kGrassTraitCount);
}

std::span<const PlantTrait> treeTraits() {
    return std::span<const PlantTrait>(kTreeTraits, kTreeTraitCount);
}

} // namespace

std::vector<PlantGenomeComponent> makeGrassSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinGrassSpecies, kMaxGrassSpecies);
    return genetics::makeSpecies(traits(), count, seed, kGrassSpeciesSalt);
}

std::vector<PlantGenomeComponent> makeTreeSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinTreeSpecies, kMaxTreeSpecies);
    return genetics::makeSpecies(treeTraits(), count, seed, kTreeSpeciesSalt);
}

PlantGenomeComponent mutateGenome(const PlantGenomeComponent& parent, const PlantGenomeComponent& archetype,
                                  float mutationRate, std::uint64_t seed) {
    return genetics::mutate(traits(), parent, archetype, mutationRate, seed);
}

PlantGenomeComponent mutateTreeGenome(const PlantGenomeComponent& parent, const PlantGenomeComponent& archetype,
                                       float mutationRate, std::uint64_t seed) {
    return genetics::mutate(treeTraits(), parent, archetype, mutationRate, seed);
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
    out.push_back({g, "kMinTreeSpecies", static_cast<float>(kMinTreeSpecies)});
    out.push_back({g, "kMaxTreeSpecies", static_cast<float>(kMaxTreeSpecies)});
}

} // namespace goblins

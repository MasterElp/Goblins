#include "core/generation/AnimalGenetics.hpp"
#include "core/Diagnostics.hpp"

#include <algorithm>

namespace goblins {

namespace {

// Своя "соль" у каждого набора видов — не та, что у видов травы
// (kGrassSpeciesSalt): на одном и том же seed мира травоядные, хищники и
// трава не должны быть одной и той же последовательностью.
constexpr std::uint64_t kHerbivoreSpeciesSalt = 0xBEA57C0DE5EED00Dull;
constexpr std::uint64_t kPredatorSpeciesSalt = 0xC1A57A1D00F5EED1ull;

} // namespace

std::vector<AnimalGenomeComponent> makeHerbivoreSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinHerbivoreSpecies, kMaxHerbivoreSpecies);
    return genetics::makeSpecies(herbivoreTraits(), count, seed, kHerbivoreSpeciesSalt);
}

std::vector<AnimalGenomeComponent> makePredatorSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinPredatorSpecies, kMaxPredatorSpecies);
    return genetics::makeSpecies(predatorTraits(), count, seed, kPredatorSpeciesSalt);
}

AnimalGenomeComponent crossGenomes(std::span<const AnimalTrait> traits, const AnimalGenomeComponent& mother,
                                    const AnimalGenomeComponent& father, const AnimalGenomeComponent& archetype,
                                    float mutationRate, std::uint64_t seed) {
    return genetics::cross(traits, mother, father, archetype, mutationRate, seed);
}

AnimalGenomeComponent mutateGenome(std::span<const AnimalTrait> traits, const AnimalGenomeComponent& parent,
                                    const AnimalGenomeComponent& archetype, float mutationRate,
                                    std::uint64_t seed) {
    return genetics::mutate(traits, parent, archetype, mutationRate, seed);
}


// Константы этих таблиц — наружу только для чтения (core/Diagnostics.hpp).
// Общая механика бюджета перечислена в группе "Genetics"
// (appendPlantGeneticsConstants) — она одна на всё живое, дублировать её
// здесь незачем.
void appendAnimalGeneticsConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Animal genetics";
    out.push_back({g, "kMinHerbivoreSpecies", static_cast<float>(kMinHerbivoreSpecies)});
    out.push_back({g, "kMaxHerbivoreSpecies", static_cast<float>(kMaxHerbivoreSpecies)});
    out.push_back({g, "kHerbivoreTraitCount", static_cast<float>(kHerbivoreTraitCount)});
    out.push_back({g, "kMinPredatorSpecies", static_cast<float>(kMinPredatorSpecies)});
    out.push_back({g, "kMaxPredatorSpecies", static_cast<float>(kMaxPredatorSpecies)});
    out.push_back({g, "kPredatorTraitCount", static_cast<float>(kPredatorTraitCount)});
}

} // namespace goblins

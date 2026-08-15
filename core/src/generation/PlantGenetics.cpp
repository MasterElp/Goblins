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

float habitatFit(const PlantGenomeComponent& genome, const SoilComponent& soil) {
    // Каменистость: чем дальше от "своей", тем хуже, и на границе допуска
    // рост прекращается совсем. Узкий допуск — специалист (в своей полосе
    // каменистости ему хорошо, на шаг в сторону уже нет), широкий —
    // универсал, который растёт почти везде, но платит за это бюджетом.
    const float rockinessMiss = std::abs(soil.rockiness - genome.preferredRockiness);
    const float rockinessFit =
        genome.rockinessTolerance > 0.0f ? std::clamp(1.0f - rockinessMiss / genome.rockinessTolerance, 0.0f, 1.0f)
                                          : 0.0f;

    // Утоптанность: до предела терпимости не мешает вовсе, дальше рост
    // падает линейно до нуля на полностью убитой почве. Односторонняя
    // черта (рыхлая почва плоха ни для кого), поэтому и формула
    // односторонняя, в отличие от каменистости.
    float compactionFit = 1.0f;
    if (soil.compaction > genome.compactionTolerance) {
        const float headroom = 1.0f - genome.compactionTolerance;
        compactionFit = headroom > 0.0f ? std::clamp(1.0f - (soil.compaction - genome.compactionTolerance) / headroom,
                                                      0.0f, 1.0f)
                                         : 0.0f;
    }

    return rockinessFit * compactionFit;
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

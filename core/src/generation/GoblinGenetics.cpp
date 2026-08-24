#include "core/generation/GoblinGenetics.hpp"
#include "core/Diagnostics.hpp"

#include <algorithm>

namespace goblins {

namespace {

// Своя "соль", как у видов травы и у двух звериных таблиц: на одном и том же
// seed мира племена гоблинов не должны оказаться той же последовательностью,
// что травоядные или хищники.
constexpr std::uint64_t kGoblinTribesSalt = 0x60B11D5EED5A1701ull;

} // namespace

std::vector<AnimalGenomeComponent> makeGoblinTribes(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinGoblinTribes, kMaxGoblinTribes);
    return genetics::makeSpecies(goblinTraits(), count, seed, kGoblinTribesSalt);
}

// Скрещивания и мутации своих у гоблина нет и не нужно: crossGenomes и
// mutateGenome (core/generation/AnimalGenetics.hpp) уже принимают таблицу
// черт параметром, и вся разница между зверем и гоблином в неё и уходит.
// Своя пара функций была бы двумя строками пересылки и третьим местом,
// которое однажды забудут поправить.

// Константы этой таблицы — наружу только для чтения (core/Diagnostics.hpp).
// Общая механика бюджета перечислена в группе "Genetics" — она одна на всё
// живое, дублировать её здесь незачем.
void appendGoblinGeneticsConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Goblin genetics";
    out.push_back({g, "kMinGoblinTribes", static_cast<float>(kMinGoblinTribes)});
    out.push_back({g, "kMaxGoblinTribes", static_cast<float>(kMaxGoblinTribes)});
    out.push_back({g, "kGoblinTraitCount", static_cast<float>(kGoblinTraitCount)});
}

} // namespace goblins

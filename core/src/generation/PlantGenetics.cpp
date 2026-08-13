#include "core/generation/PlantGenetics.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace goblins {

namespace {

// Вложение не должно вырождаться: черта на нуле — это гарантированно
// нежизнеспособный вид (например, seedChance ~ 0 — вид не оставит
// потомства вовсе), черта на единице — вид, упирающийся в край диапазона,
// которому мутация может двигаться только в одну сторону.
constexpr float kMinAdvantage = 0.05f;
constexpr float kMaxAdvantage = 0.95f;

// Подгонка вложений под бюджет — итеративная (масштабировать -> обрезать
// по краям -> повторить), потому что обрезка краёв сама сбивает сумму.
// Нескольких проходов достаточно: остаточная ошибка бюджета меньше, чем
// разброс между видами, и на баланс не влияет.
constexpr int kBudgetFitIterations = 8;

// Минимальная средняя разница вложений между двумя видами — иначе в мире
// оказались бы две почти одинаковые травы, и "несколько видов" ничего бы
// не значило. Попытки ограничены, как и везде в генерации
// (ср. BoulderScatter/TerrainGenerator): при 12 видах место в пространстве
// стратегий рано или поздно кончается, и упереться в лимит попыток — это
// нормально, а не ошибка.
constexpr float kMinSpeciesDistance = 0.1f;
constexpr int kSpeciesAttempts = 200;

using Allocation = std::array<float, kGrassTraitCount>;

std::uint64_t nextState(std::uint64_t& state) {
    // splitmix64: дешёвый и без состояния между вызовами, кроме самого
    // state — ровно то, что нужно системе, которой запрещено хранить
    // состояние между тиками (05_Entity.md, п.3).
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Случайная величина, распределённая как Gamma(2): даёт неравномерный, но
// и не вырожденный расклад бюджета — обычно у вида два-три конька и
// несколько слабых мест, а не "всё поровну" (как дало бы равномерное) и
// не "всё в одну черту" (как дало бы экспоненциальное).
float randomGamma2(std::uint64_t& state) {
    const float u1 = std::max(randomUnit(state), 1e-6f);
    const float u2 = std::max(randomUnit(state), 1e-6f);
    return -std::log(u1 * u2);
}

float pricedBudget() {
    float totalWeight = 0.0f;
    for (const auto& trait : kGrassTraits) {
        totalWeight += trait.weight;
    }
    return totalWeight * kAdvantageBudgetShare;
}

// Приводит вложения к общему бюджету: Σ weight * a == budget, каждое a в
// [kMinAdvantage, kMaxAdvantage]. Бесплатные черты (weight == 0) не
// трогаются вовсе — они не участвуют в бюджете и вольны быть любыми.
void fitToBudget(Allocation& allocation) {
    const float budget = pricedBudget();
    for (int iteration = 0; iteration < kBudgetFitIterations; ++iteration) {
        float spent = 0.0f;
        for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
            spent += kGrassTraits[t].weight * allocation[t];
        }
        if (spent <= 0.0f) {
            return;
        }
        const float scale = budget / spent;
        for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
            if (kGrassTraits[t].weight <= 0.0f) {
                continue;
            }
            allocation[t] = std::clamp(allocation[t] * scale, kMinAdvantage, kMaxAdvantage);
        }
    }
}

float advantageOf(const PlantGenomeComponent& genome, const PlantTrait& trait) {
    const float span = trait.atOne - trait.atZero;
    if (span == 0.0f) {
        return 0.0f;
    }
    return std::clamp((genome.*trait.gene - trait.atZero) / span, 0.0f, 1.0f);
}

Allocation allocationOf(const PlantGenomeComponent& genome) {
    Allocation allocation{};
    for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
        allocation[t] = advantageOf(genome, kGrassTraits[t]);
    }
    return allocation;
}

PlantGenomeComponent genomeOf(const Allocation& allocation) {
    PlantGenomeComponent genome;
    for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
        const auto& trait = kGrassTraits[t];
        genome.*trait.gene = trait.atZero + (trait.atOne - trait.atZero) * allocation[t];
    }
    return genome;
}

// Насколько два вида различаются: средняя разница вложений по платным
// чертам плюс разница ниш. Ниша учитывается наравне с остальным — две
// травы с одинаковой стратегией, но разной "своей" каменистостью, это
// всё-таки разные травы, они растут в разных местах карты.
float speciesDistance(const Allocation& a, const Allocation& b) {
    float sum = 0.0f;
    for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
        sum += std::abs(a[t] - b[t]);
    }
    return sum / static_cast<float>(kGrassTraitCount);
}

} // namespace

std::uint64_t mixSeed(std::uint64_t a, std::uint64_t b) {
    std::uint64_t state = a ^ (b * 0x9E3779B97F4A7C15ull);
    return nextState(state);
}

float randomUnit(std::uint64_t& state) {
    // 24 старших бита -> [0, 1): float всё равно не различает больше.
    return static_cast<float>(nextState(state) >> 40) * (1.0f / 16777216.0f);
}

std::vector<PlantGenomeComponent> makeGrassSpecies(int count, std::uint64_t seed) {
    count = std::clamp(count, kMinGrassSpecies, kMaxGrassSpecies);

    // Отдельная "соль" от той, что берёт мутация ниже: набор видов и
    // мутации отдельного семечка не должны быть одной и той же
    // последовательностью при одинаковом seed.
    std::uint64_t state = mixSeed(seed, 0xD1CE5EEDA5510C11ull);

    std::vector<Allocation> accepted;
    accepted.reserve(static_cast<std::size_t>(count));

    std::vector<PlantGenomeComponent> species;
    species.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        Allocation allocation{};
        for (int attempt = 0; attempt < kSpeciesAttempts; ++attempt) {
            for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
                // Бесплатная черта — не расклад бюджета, а просто выбор
                // ниши: равномерно по всему диапазону.
                allocation[t] = kGrassTraits[t].weight > 0.0f ? randomGamma2(state) : randomUnit(state);
            }
            fitToBudget(allocation);

            const bool distinct =
                std::none_of(accepted.begin(), accepted.end(), [&](const Allocation& other) {
                    return speciesDistance(allocation, other) < kMinSpeciesDistance;
                });
            if (distinct) {
                break;
            }
            // Не получилось за все попытки — берём последний расклад как
            // есть: лучше чуть похожий вид, чем на вид беспричинно
            // пропавший (мир обязан получить ровно столько видов, сколько
            // заказано в настройках генерации).
        }

        accepted.push_back(allocation);

        PlantGenomeComponent genome = genomeOf(allocation);
        genome.species = index;
        species.push_back(genome);
    }

    return species;
}

PlantGenomeComponent mutateGenome(const PlantGenomeComponent& parent, const PlantGenomeComponent& archetype,
                                  float mutationRate, std::uint64_t seed) {
    Allocation allocation = allocationOf(parent);
    const Allocation center = allocationOf(archetype);

    std::uint64_t state = mixSeed(seed, 0x9E3779B97F4A7C15ull);
    for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
        const float shift = (randomUnit(state) * 2.0f - 1.0f) * mutationRate;
        allocation[t] = std::clamp(allocation[t] + shift, 0.0f, 1.0f);
    }

    // Полоса вокруг архетипа и общий бюджет — два ограничения, которые
    // мешают друг другу (подгонка бюджета выталкивает из полосы, обрезка
    // по полосе сбивает бюджет), поэтому применяются по очереди несколько
    // раз. Полоса остаётся последней: важнее, чтобы вид не расплылся, чем
    // чтобы бюджет сошёлся до последнего знака.
    for (int iteration = 0; iteration < 3; ++iteration) {
        fitToBudget(allocation);
        for (std::size_t t = 0; t < kGrassTraitCount; ++t) {
            allocation[t] = std::clamp(allocation[t], center[t] - kSpeciesBand, center[t] + kSpeciesBand);
            allocation[t] = std::clamp(allocation[t], 0.0f, 1.0f);
        }
    }

    PlantGenomeComponent child = genomeOf(allocation);
    child.species = parent.species;
    return child;
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

} // namespace goblins

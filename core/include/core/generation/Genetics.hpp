#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/Random.hpp"

namespace goblins::genetics {

// Механика "таблица черт + бюджет преимуществ" — общая для всего живого,
// а не свойство растений.
//
// Она появилась вместе с травой и была описана в docs/08_Plants.md, п.2–4
// обещанием: "Новое растение (не трава) — это другая таблица черт при той
// же механике". С появлением травоядных обещание пришлось выполнить
// буквально: код разделён на механику (этот файл) и таблицы черт
// (core/generation/PlantGenetics.hpp, core/generation/HerbivoreGenetics.hpp).
// Скопировать полторы сотни строк балансировки во второй раз было нельзя по
// той же причине, по которой формула влажности не может быть написана
// дважды (core/Moisture.hpp): два экземпляра одного закона неизбежно
// разъезжаются, и разъезжаются молча.
//
// --- Как устроен баланс ---
//
// Каждая черта выражается не сразу числом, а "вложением" a в диапазоне
// [0, 1]: a=0 — худшее для существа значение черты (atZero), a=1 — лучшее
// (atOne). Направление зашито в саму пару: у возраста размножения
// atZero — поздно, atOne — рано; у предельного возраста наоборот. Черта
// стоит weight за единицу вложения, и сумма Σ weight * a у ВСЕХ видов
// одинакова (kAdvantageBudgetShare от полной цены) — вид не может быть
// лучше другого во всём, он может быть только лучше в одном за счёт
// другого. Отсюда сами собой получаются стратегии вида "быстрый рост ->
// короткая жизнь", и задавать их списком не требуется.
//
// Черта с weight = 0 — не преимущество, а выбор ниши: ни одно её значение
// не лучше другого, оно лишь определяет, ГДЕ виду хорошо.
//
// Требование к типу Genome: поле `int species` (индекс архетипа) и
// int-поля, на которые ссылаются черты. Больше механика о существе ничего
// не знает — ни что оно ест, ни как размножается.
//
// Гены целые, как и всё остальное состояние мира (core/Scale.hpp), а
// расклад бюджета внутри этого файла остаётся дробным — он не состояние, а
// способ вывести значения генов один раз при рождении. Дробное становится
// целым ровно на границе: в genomeOf, при записи в геном.

template <typename Genome>
struct Trait {
    // Имя черты в JSON — под ним геном уходит в файл мира и в снапшот
    // клиенту (server/WorldSave.cpp, server/NetworkServer.cpp).
    const char* name;
    // Поле генома, которым эта черта является.
    int Genome::* gene;
    // Значение гена при нулевом и при полном вложении. Порядок задаёт
    // направление: atZero всегда "хуже для существа", atOne — "лучше".
    int atZero;
    int atOne;
    // Цена единицы вложения. 0 — черта бесплатна (выбор ниши, а не
    // преимущество) и в бюджете не участвует.
    float weight;
};

// Вложение не должно вырождаться: черта на нуле — это гарантированно
// нежизнеспособный вид (нулевая плодовитость — вид без потомства), черта на
// единице — вид, упирающийся в край диапазона, которому мутация может
// двигаться только в одну сторону.
inline constexpr float kMinAdvantage = 0.05f;
inline constexpr float kMaxAdvantage = 0.95f;

// Подгонка вложений под бюджет — итеративная (масштабировать -> обрезать
// по краям -> повторить), потому что обрезка краёв сама сбивает сумму.
// Нескольких проходов достаточно: остаточная ошибка бюджета меньше, чем
// разброс между видами, и на баланс не влияет.
inline constexpr int kBudgetFitIterations = 8;

// Минимальная средняя разница вложений между двумя видами — иначе в мире
// оказались бы два почти одинаковых вида, и "несколько видов" ничего бы не
// значило. Попытки ограничены, как и везде в генерации: при большом числе
// видов место в пространстве стратегий рано или поздно кончается, и
// упереться в лимит попыток — это нормально, а не ошибка.
inline constexpr float kMinSpeciesDistance = 0.1f;
inline constexpr int kSpeciesAttempts = 200;

// Доля полной цены всех платных черт, которую тратит КАЖДЫЙ вид. 0.5
// означало бы "средний вид ровно посередине по всем чертам"; чуть меньше
// половины оставляет запас на мутации в обе стороны, не давая виду
// упереться в потолок диапазонов.
inline constexpr float kAdvantageBudgetShare = 0.45f;

// Насколько потомку разрешено отклоняться от архетипа своего вида (в долях
// вложения a). Это и есть ответ на "как избежать постепенного ухода
// параметров в экстремальные значения": мутация случайна, но заперта в
// полосе вокруг архетипа, поэтому вид дрейфует внутри своей стратегии, а не
// уползает поколение за поколением к краю диапазона.
inline constexpr float kSpeciesBand = 0.12f;

// Расклад бюджета по чертам — по вложению на каждую черту таблицы.
using Allocation = std::vector<float>;

namespace detail {

// Случайная величина, распределённая как Gamma(2): даёт неравномерный, но и
// не вырожденный расклад бюджета — обычно у вида два-три конька и несколько
// слабых мест, а не "всё поровну" (как дало бы равномерное) и не "всё в одну
// черту" (как дало бы экспоненциальное).
inline float randomGamma2(std::uint64_t& state) {
    const float u1 = std::max(randomUnit(state), 1e-6f);
    const float u2 = std::max(randomUnit(state), 1e-6f);
    return -std::log(u1 * u2);
}

template <typename Genome>
float pricedBudget(std::span<const Trait<Genome>> traits) {
    float totalWeight = 0.0f;
    for (const auto& trait : traits) {
        totalWeight += trait.weight;
    }
    return totalWeight * kAdvantageBudgetShare;
}

// Приводит вложения к общему бюджету: Σ weight * a == budget, каждое a в
// [kMinAdvantage, kMaxAdvantage]. Бесплатные черты (weight == 0) не
// трогаются вовсе — они не участвуют в бюджете и вольны быть любыми.
template <typename Genome>
void fitToBudget(std::span<const Trait<Genome>> traits, Allocation& allocation) {
    const float budget = pricedBudget(traits);
    for (int iteration = 0; iteration < kBudgetFitIterations; ++iteration) {
        float spent = 0.0f;
        for (std::size_t t = 0; t < traits.size(); ++t) {
            spent += traits[t].weight * allocation[t];
        }
        if (spent <= 0.0f) {
            return;
        }
        const float scale = budget / spent;
        for (std::size_t t = 0; t < traits.size(); ++t) {
            if (traits[t].weight <= 0.0f) {
                continue;
            }
            allocation[t] = std::clamp(allocation[t] * scale, kMinAdvantage, kMaxAdvantage);
        }
    }
}

template <typename Genome>
float advantageOf(const Genome& genome, const Trait<Genome>& trait) {
    const int span = trait.atOne - trait.atZero;
    if (span == 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(genome.*trait.gene - trait.atZero) / static_cast<float>(span), 0.0f, 1.0f);
}

// Насколько два вида различаются: средняя разница вложений по всем чертам,
// включая бесплатные. Ниша учитывается наравне с остальным — два вида с
// одинаковой стратегией, но разными нишами, это всё-таки разные виды.
inline float speciesDistance(const Allocation& a, const Allocation& b) {
    float sum = 0.0f;
    for (std::size_t t = 0; t < a.size(); ++t) {
        sum += std::abs(a[t] - b[t]);
    }
    return a.empty() ? 0.0f : sum / static_cast<float>(a.size());
}

} // namespace detail

// Вложения генома — обратный перевод "значения генов -> расклад бюджета".
template <typename Genome>
Allocation allocationOf(std::span<const Trait<Genome>> traits, const Genome& genome) {
    Allocation allocation(traits.size(), 0.0f);
    for (std::size_t t = 0; t < traits.size(); ++t) {
        allocation[t] = detail::advantageOf(genome, traits[t]);
    }
    return allocation;
}

// Геном по раскладу бюджета. species не выставляется — его знает
// вызывающая сторона.
//
// Здесь и только здесь дробный расклад становится целыми генами. Округляем
// к ближайшему, а не отбрасываем дробную часть: отбрасывание всегда
// сдвигало бы вид в сторону atZero, то есть в сторону "хуже", и тем
// сильнее, чем мельче шкала черты.
template <typename Genome>
Genome genomeOf(std::span<const Trait<Genome>> traits, const Allocation& allocation) {
    Genome genome;
    for (std::size_t t = 0; t < traits.size(); ++t) {
        const auto& trait = traits[t];
        const float span = static_cast<float>(trait.atOne - trait.atZero);
        genome.*trait.gene = trait.atZero + static_cast<int>(std::lround(span * allocation[t]));
    }
    return genome;
}

// Набор архетипов: count видов, у каждого — свой расклад одного и того же
// бюджета преимуществ. Виды, оказавшиеся слишком похожими на уже созданные,
// перевыбираются, чтобы в мире не было двух почти одинаковых видов.
//
// salt отличает поток случайности этого набора от других розыгрышей на том
// же seed мира (виды травы, виды травоядных, мутации отдельного потомка):
// значение ничего не означает, кроме "не совпадать".
template <typename Genome>
std::vector<Genome> makeSpecies(std::span<const Trait<Genome>> traits, int count, std::uint64_t seed,
                                std::uint64_t salt) {
    std::uint64_t state = mixSeed(seed, salt);

    std::vector<Allocation> accepted;
    accepted.reserve(static_cast<std::size_t>(std::max(0, count)));

    std::vector<Genome> species;
    species.reserve(static_cast<std::size_t>(std::max(0, count)));

    for (int index = 0; index < count; ++index) {
        Allocation allocation(traits.size(), 0.0f);
        for (int attempt = 0; attempt < kSpeciesAttempts; ++attempt) {
            for (std::size_t t = 0; t < traits.size(); ++t) {
                // Бесплатная черта — не расклад бюджета, а просто выбор
                // ниши: равномерно по всему диапазону.
                allocation[t] = traits[t].weight > 0.0f ? detail::randomGamma2(state) : randomUnit(state);
            }
            detail::fitToBudget(traits, allocation);

            const bool distinct = std::none_of(accepted.begin(), accepted.end(), [&](const Allocation& other) {
                return detail::speciesDistance(allocation, other) < kMinSpeciesDistance;
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

        Genome genome = genomeOf(traits, allocation);
        genome.species = index;
        species.push_back(genome);
    }

    return species;
}

// Общий хвост наследования: помутировать расклад, прижать его к полосе
// вокруг архетипа и вернуть на общий бюджет.
//
// Пересчёт бюджета важен не меньше самой мутации: без него удачная мутация
// просто добавляла бы потомку преимуществ, и за несколько сотен поколений
// линия стала бы лучше всех во всём. С ним любая мутация — это обмен: где-то
// прибавилось, значит где-то убыло.
//
// Полоса и бюджет мешают друг другу (подгонка бюджета выталкивает из полосы,
// обрезка по полосе сбивает бюджет), поэтому применяются по очереди
// несколько раз. Полоса остаётся последней: важнее, чтобы вид не расплылся,
// чем чтобы бюджет сошёлся до последнего знака.
template <typename Genome>
Genome finishOffspring(std::span<const Trait<Genome>> traits, Allocation allocation, const Genome& archetype,
                       float mutationRate, std::uint64_t& state) {
    const Allocation center = allocationOf(traits, archetype);

    for (std::size_t t = 0; t < traits.size(); ++t) {
        const float shift = (randomUnit(state) * 2.0f - 1.0f) * mutationRate;
        allocation[t] = std::clamp(allocation[t] + shift, 0.0f, 1.0f);
    }

    for (int iteration = 0; iteration < 3; ++iteration) {
        detail::fitToBudget(traits, allocation);
        for (std::size_t t = 0; t < traits.size(); ++t) {
            allocation[t] = std::clamp(allocation[t], center[t] - kSpeciesBand, center[t] + kSpeciesBand);
            allocation[t] = std::clamp(allocation[t], 0.0f, 1.0f);
        }
    }

    return genomeOf(traits, allocation);
}

// Геном потомка от ОДНОГО родителя: родительский расклад, слегка сдвинутый
// мутацией. Так размножается трава — семя падает с одного растения.
template <typename Genome>
Genome mutate(std::span<const Trait<Genome>> traits, const Genome& parent, const Genome& archetype,
              float mutationRate, std::uint64_t seed) {
    std::uint64_t state = mixSeed(seed, 0x9E3779B97F4A7C15ull);
    Genome child = finishOffspring(traits, allocationOf(traits, parent), archetype, mutationRate, state);
    child.species = parent.species;
    return child;
}

// Геном потомка от ДВУХ родителей: по каждой черте берётся вложение того или
// другого (равномерное скрещивание), дальше — та же мутация и тот же бюджет.
//
// Именно почерпнутая целиком черта, а не среднее двух: среднее за несколько
// поколений сгладило бы вид в один усреднённый расклад, и два пола перестали
// бы что-либо значить — потомок был бы просто размытым родителем. При выборе
// же по одной черте потомок получает узнаваемые куски обоих, а разнообразие
// внутри вида поддерживается самим фактом встречи разных линий.
template <typename Genome>
Genome cross(std::span<const Trait<Genome>> traits, const Genome& first, const Genome& second,
             const Genome& archetype, float mutationRate, std::uint64_t seed) {
    std::uint64_t state = mixSeed(seed, 0xC0FFEE5EEDBEEF11ull);

    const Allocation a = allocationOf(traits, first);
    const Allocation b = allocationOf(traits, second);
    Allocation mixed(traits.size(), 0.0f);
    for (std::size_t t = 0; t < traits.size(); ++t) {
        mixed[t] = randomUnit(state) < 0.5f ? a[t] : b[t];
    }

    Genome child = finishOffspring(traits, std::move(mixed), archetype, mutationRate, state);
    child.species = first.species;
    return child;
}

} // namespace goblins::genetics

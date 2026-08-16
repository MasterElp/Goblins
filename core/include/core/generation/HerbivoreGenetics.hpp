#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/Random.hpp"
#include "core/components/HerbivoreGenomeComponent.hpp"
#include "core/generation/Genetics.hpp"

namespace goblins {

// Таблица черт травоядного и порождение его генома — ровно то же место в
// мире, что PlantGenetics.hpp занимает для травы: общий для двух вызывающих
// сторон набор чистых функций (генерация расселяет первое поголовье,
// HerbivoreSystem порождает телят при встрече родителей).
//
// Механика — общая (core/generation/Genetics.hpp): бюджет преимуществ,
// полоса вида, мутация как обмен. Здесь только то, что относится именно к
// травоядному: какие у него черты, в каких пределах они живут и сколько
// стоят.
using HerbivoreTrait = genetics::Trait<HerbivoreGenomeComponent>;

// Числа подобраны под уже существующий мир, а не сами по себе:
// - развитость растения (PlantComponent::growth) живёт в [0, 1], поэтому
//   bite_size — доля этой развитости за тик, а энергия считается из неё
//   через kEnergyPerBiomass (HerbivoreSystem);
// - белок — те же счётные крупицы, что SoilComponent.minerals: у травы их
//   единицы, поэтому и protein_need измеряется единицами;
// - времена — в тиках, и они намеренно на порядок больше растительных:
//   трава живёт 300..2500 тиков, травоядное — 3000..20000, то есть
//   переживает несколько поколений той травы, которой кормится. Иначе
//   стадо было бы просто "медленной травой" и никакой разницы темпов в
//   мире не появилось бы.
//
// Цены (weight) не равны между собой по той же причине, что и у травы:
// плодовитость — самое сильное преимущество в мире, где еда конечна,
// поэтому breeding_urge стоит втрое, а долгая жизнь — в полтора раза
// дороже прочего. Вид, купивший сразу и частые роды, и долгую жизнь, и
// раннее взросление, не может позволить себе больше почти ничего: он
// плодовит, но медлителен, близорук и прожорлив.
inline constexpr HerbivoreTrait kHerbivoreTraits[] = {
    // Возраст взросления: он же срок, за который телёнок дорастает до
    // взрослого размера. Раньше повзрослел — раньше начал приносить
    // потомство и перестал быть беззащитно мелким.
    {"maturity_age", &HerbivoreGenomeComponent::maturityAge, 3000.0f, 600.0f, 1.25f},
    {"max_age", &HerbivoreGenomeComponent::maxAge, 3000.0f, 20000.0f, 1.5f},
    // Клеток за тик. Быстрый первым доходит до корма, до воды и до пары —
    // но и энергии на шаги тратит больше (kStepEnergy в HerbivoreSystem),
    // поэтому скорость платится дважды: бюджетом и обменом веществ.
    {"speed", &HerbivoreGenomeComponent::speed, 0.15f, 1.0f, 1.0f},
    // Дальность восприятия в клетках. Верх диапазона — заметно меньше
    // Области: даже самое зоркое травоядное видит свой угол карты, а не
    // весь мир, иначе понятие "искать" потеряло бы смысл.
    {"perception", &HerbivoreGenomeComponent::perception, 2.0f, 14.0f, 1.0f},
    {"energy_capacity", &HerbivoreGenomeComponent::energyCapacity, 20.0f, 140.0f, 0.75f},
    {"energy_upkeep", &HerbivoreGenomeComponent::energyUpkeep, 0.30f, 0.03f, 1.25f},
    {"bite_size", &HerbivoreGenomeComponent::biteSize, 0.05f, 0.45f, 0.75f},
    {"digestion", &HerbivoreGenomeComponent::digestion, 0.30f, 1.0f, 1.0f},
    {"water_capacity", &HerbivoreGenomeComponent::waterCapacity, 8.0f, 70.0f, 0.75f},
    {"water_upkeep", &HerbivoreGenomeComponent::waterUpkeep, 0.25f, 0.02f, 1.0f},
    // Сколько крупиц нужно на полный рост: чем меньше, тем дешевле
    // обходится собственное тело и тем быстрее телёнок становится взрослым.
    {"protein_need", &HerbivoreGenomeComponent::proteinNeed, 9.0f, 2.0f, 1.0f},
    // Насколько быстро копится желание пары у сытого взрослого. Как и
    // seed_chance у травы — самая дорогая черта в таблице.
    {"breeding_urge", &HerbivoreGenomeComponent::breedingUrge, 0.002f, 0.02f, 3.0f},
};

inline constexpr std::size_t kHerbivoreTraitCount = sizeof(kHerbivoreTraits) / sizeof(kHerbivoreTraits[0]);

// Сколько видов травоядных может быть в мире. Нижняя граница — один: в
// отличие от травы, где несколько видов нужны, чтобы было кому
// конкурировать за клетки, одинокий вид травоядных — вполне осмысленный
// мир. Верхняя — восемь: при фиксированном бюджете больше видов перестают
// быть различимыми, а поголовье каждого становится слишком малым, чтобы
// животные вообще находили друг друга для размножения.
inline constexpr int kMinHerbivoreSpecies = 1;
inline constexpr int kMaxHerbivoreSpecies = 8;

// Набор архетипов видов травоядных: count видов (обрезается до
// [kMinHerbivoreSpecies, kMaxHerbivoreSpecies]), у каждого — свой расклад
// одного и того же бюджета преимуществ.
std::vector<HerbivoreGenomeComponent> makeHerbivoreSpecies(int count, std::uint64_t seed);

// Геном телёнка: по каждой черте берётся вложение матери или отца, дальше
// мутация, полоса вида и общий бюджет (см. genetics::cross). Два пола
// существуют именно ради этого: потомок собирается из двух линий, а не
// повторяет одну с шумом.
HerbivoreGenomeComponent crossHerbivoreGenomes(const HerbivoreGenomeComponent& mother,
                                                const HerbivoreGenomeComponent& father,
                                                const HerbivoreGenomeComponent& archetype, float mutationRate,
                                                std::uint64_t seed);

// Геном отдельной особи первого поголовья: архетип вида плюс та же
// мутация, что действует и при размножении, — генетическая вариативность
// внутри вида существует с первого тика, а не появляется через поколения.
HerbivoreGenomeComponent mutateHerbivoreGenome(const HerbivoreGenomeComponent& parent,
                                                const HerbivoreGenomeComponent& archetype, float mutationRate,
                                                std::uint64_t seed);

} // namespace goblins

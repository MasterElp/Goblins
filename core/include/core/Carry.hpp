#pragma once

#include <algorithm>

#include "core/Portion.hpp"
#include "core/Resources.hpp"
#include "core/Scale.hpp"
#include "core/components/CarriedComponent.hpp"

namespace goblins {

// Ноша: сколько влезает в руки и во что обходится нести.
//
// Закон общий для всего живого и принимает числа, а не существ (образец —
// core/Rest.hpp, core/Berries.hpp): унести кусок может и зверь, и когда у
// зверя появится причина это делать, здесь не изменится ни строки.
//
// Смысл ноши не в том, что вещь можно взять с собой, а в том, что её можно
// ПРИНЕСТИ. Съеденное там, где найдено, не оставляет в мире следа;
// принесённое собирается в кучу, а куча даёт месту вторую причину
// существовать (core/Store.hpp).

// Сколько всего помещается в руки у существа полного размера. Десяток ягод
// (kBerryMass = 200, core/Berries.hpp) или четверть туши: горсть, а не
// телега. Больше — и один поход к ягоднику кормил бы лагерь неделю, то есть
// ходить было бы незачем.
constexpr int kCarryPerSize = 2000;

// Во что обходится полная ноша: шаг с ней стоит вдвое против пустых рук.
//
// Цена обязательна, и вот почему. Без неё ноша — бесплатное улучшение, а
// запас — чистая прибыль: носить всегда выгоднее, чем не носить, и решать тут
// нечего. С ценой появляется выбор, которого гоблин не делает разумом, но
// который делает за него мир: дальний ягодник окупается хуже ближнего, а
// лагерь, стоящий далеко от еды, проигрывает лагерю, стоящему рядом.
constexpr int kCarryStepCost = 1000;

// Сколько влезает в руки существу такого размера.
inline int carryCapacity(int size) {
    return kCarryPerSize * std::clamp(size, 0, kFull) / kFull;
}

// Сколько ещё влезет — общим счётом на все виды: руки одни (core/Resources.hpp).
inline int carryRoom(const CarriedComponent& hands, int size) {
    return std::max(0, carryCapacity(size) - hands.carried.total());
}

// Насколько полны руки, 0..kFull. Из этого считается и цена шага, и срочность
// желания отнести (GoblinSystem).
inline int carryLoad(const CarriedComponent& hands, int size) {
    const int capacity = carryCapacity(size);
    return capacity > 0 ? std::clamp(hands.carried.total() * kFull / capacity, 0, kFull) : 0;
}

// Цена шага с ношей: base — то, что шаг стоил бы с пустыми руками.
inline int carryStepEnergy(int base, const CarriedComponent& hands, int size) {
    return base + base * carryLoad(hands, size) * kCarryStepCost / kFull / kFull;
}

// Взять в руки. Больше, чем влезает, не взять: возвращает принятое, остальное
// остаётся там, откуда брали.
inline Portion putInHands(CarriedComponent& hands, ResourceKind kind, Portion what, int size) {
    return addResource(hands.carried, kind, what, carryRoom(hands, size));
}

// Взять из рук — съесть, положить в кучу или пустить в дело. Крупицы уходят
// той же долей (core/Portion.hpp), и только у еды они и есть.
inline Portion takeFromHands(CarriedComponent& hands, ResourceKind kind, int want) {
    return takeResource(hands.carried, kind, want);
}

} // namespace goblins

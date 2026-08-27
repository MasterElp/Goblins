#pragma once

#include <algorithm>

#include "core/Portion.hpp"
#include "core/Scale.hpp"
#include "core/components/CarriedComponent.hpp"

namespace goblins {

// Ноша: сколько влезает в руки и во что обходится нести.
//
// Закон общий для всего живого и принимает числа, а не существ (образец —
// core/Rest.hpp, core/Berries.hpp): унести кусок может и зверь, и когда у
// зверя появится причина это делать, здесь не изменится ни строки.
//
// Смысл ноши не в том, что еду можно взять с собой, а в том, что её можно
// ПРИНЕСТИ. Съеденное там, где найдено, не оставляет в мире следа; принесённое
// собирается в кучу, а куча даёт месту вторую причину существовать — к ней
// возвращаются и голодные, и сытые (см. core/Store.hpp).

// Сколько еды помещается в руки у существа полного размера. Десяток ягод
// (kBerryMass = 200, core/Berries.hpp) или четверть туши: горсть, а не
// телега. Больше — и один поход к ягоднику кормил бы лагерь неделю, то есть
// ходить было бы незачем.
constexpr int kCarryPerSize = 2000;

// Во что обходится полная ноша: шаг с ней стоит вдвое против пустых рук.
//
// Цена обязательна, и вот почему. Без неё ноша — бесплатное улучшение, а
// запас — чистая прибыль: носить всегда выгоднее, чем не носить, и решать
// тут нечего. С ценой появляется выбор, которого гоблин не делает разумом, но
// который делает за него мир: дальний ягодник окупается хуже ближнего, а
// лагерь, стоящий далеко от еды, проигрывает лагерю, стоящему рядом.
constexpr int kCarryStepCost = 1000;

// Сколько влезает в руки существу такого размера.
inline int carryCapacity(int size) {
    return kCarryPerSize * std::clamp(size, 0, kFull) / kFull;
}

// Сколько всего в руках: и еда, и материал. Руки одни, и это не мелочь —
// набравший полные руки веток не унесёт с собой ничего съестного, а значит,
// за материалом ходят ОТДЕЛЬНЫМ походом. Стройка стоит не только труда, но и
// того времени, которое не потрачено на еду.
//
// Ветка занимает столько же места, сколько соломина: в руках она мерится
// объёмом, а не пользой. Втрое ценнее она на площадке, а не по дороге к ней
// (core/Build.hpp).
inline int carried(const CarriedComponent& hands) {
    return hands.food + hands.straw + hands.twigs;
}

// Сколько ещё влезет.
inline int carryRoom(const CarriedComponent& hands, int size) {
    return std::max(0, carryCapacity(size) - carried(hands));
}

// Насколько полны руки, 0..kFull. Из этого считается и цена шага, и
// срочность желания отнести (GoblinSystem).
inline int carryLoad(const CarriedComponent& hands, int size) {
    const int capacity = carryCapacity(size);
    return capacity > 0 ? std::clamp(carried(hands) * kFull / capacity, 0, kFull) : 0;
}

// Цена шага с ношей: base — то, что шаг стоил бы с пустыми руками.
inline int carryStepEnergy(int base, const CarriedComponent& hands, int size) {
    return base + base * carryLoad(hands, size) * kCarryStepCost / kFull / kFull;
}

// Взять в руки. Больше, чем влезает, взять нельзя: возвращает, сколько
// действительно взято, — остальное остаётся там, откуда брали.
inline Portion putInHands(CarriedComponent& hands, int& food, int& minerals, int want, int size) {
    const Portion taken = takePortion(food, minerals, std::min(want, carryRoom(hands, size)));
    hands.food += taken.amount;
    hands.minerals += taken.minerals;
    return taken;
}

// Взять из рук ЕДУ — съесть или положить в кучу. Крупицы уходят той же долей
// (core/Portion.hpp). Материал этим не берётся: его не едят и кладут не в
// кучу, а в стройку.
inline Portion takeFromHands(CarriedComponent& hands, int want) {
    return takePortion(hands.food, hands.minerals, want);
}

// Набрать материала в руки. Возвращает, сколько взято: больше, чем влезает,
// не взять, и остальное остаётся на растении.
inline int takeStraw(CarriedComponent& hands, int want, int size) {
    const int taken = std::max(0, std::min(want, carryRoom(hands, size)));
    hands.straw += taken;
    return taken;
}

inline int takeTwigs(CarriedComponent& hands, int want, int size) {
    const int taken = std::max(0, std::min(want, carryRoom(hands, size)));
    hands.twigs += taken;
    return taken;
}

} // namespace goblins

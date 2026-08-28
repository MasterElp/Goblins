#pragma once

#include <algorithm>

#include "core/Body.hpp"
#include "core/Scale.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"

namespace goblins {

// Голод и жажда — ОДИН закон мира на систему и на наблюдателя.
//
// Оба читаются из тела и ниоткуда больше: голод — это пустеющий запас
// энергии и нехватка белка, жажда — пустеющий запас воды. Поэтому они и не
// хранятся в компоненте (см. DesireComponent): хранить нечего, всё уже
// лежит в AnimalComponent, а пересчёт стоит двух делений.
//
// Живут эти две формулы здесь, а не внутри AnimalSystem, по той же причине,
// что и moistureTarget в core/Moisture.hpp: пользуются ими уже двое —
// система (чтобы выбрать желание) и сервер (чтобы показать животное в
// панели наблюдения), — а две копии одного закона неизбежно разъезжаются, и
// разъезжаются молча. Панель, показывающая не тот голод, по которому
// животное приняло решение, хуже панели, не показывающей ничего.
//
// Страха здесь нет и быть не может: он считается не из своего тела, а из
// чужого присутствия — кто виден отсюда и насколько близко, — и знать это
// может только тот, у кого перед глазами весь снимок тика. Он и остаётся
// внутри AnimalSystem.

// Голод, 0..kFull. Берётся худшее из двух: пустой желудок и нехватка белка
// на собственный рост — это один и тот же позыв есть, и удовлетворяются они
// одной и той же травой.
inline int hungerOf(const AnimalComponent& state, const AnimalGenomeComponent& genome) {
    const int energyDeficit = kFull - state.energy * kFull / energyCapacityOf(genome);
    const int proteinDeficit = kFull - state.protein * kFull / proteinNeedOf(genome);
    return std::clamp(std::max(energyDeficit, proteinDeficit), 0, kFull);
}

// Жажда, 0..kFull.
inline int thirstOf(const AnimalComponent& state, const AnimalGenomeComponent& genome) {
    return std::clamp(kFull - state.water * kFull / waterCapacityOf(genome), 0, kFull);
}

} // namespace goblins

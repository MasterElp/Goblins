#pragma once

#include <algorithm>

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

// Голод, 0..1. Берётся худшее из двух: пустой желудок и нехватка белка на
// собственный рост — это один и тот же позыв есть, и удовлетворяются они
// одной и той же травой.
inline float hungerOf(const AnimalComponent& state, const AnimalGenomeComponent& genome) {
    const float energyDeficit = genome.energyCapacity > 0.0f ? 1.0f - state.energy / genome.energyCapacity : 1.0f;
    const float proteinDeficit =
        genome.proteinNeed > 0.0f ? 1.0f - static_cast<float>(state.protein) / genome.proteinNeed : 0.0f;
    return std::clamp(std::max(energyDeficit, proteinDeficit), 0.0f, 1.0f);
}

// Жажда, 0..1.
inline float thirstOf(const AnimalComponent& state, const AnimalGenomeComponent& genome) {
    return std::clamp(genome.waterCapacity > 0.0f ? 1.0f - state.water / genome.waterCapacity : 1.0f, 0.0f, 1.0f);
}

} // namespace goblins

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/Portion.hpp"
#include "core/Scale.hpp"
#include "core/World.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/StoreComponent.hpp"

namespace goblins {

// Куча: как принесённое ложится на землю и как оно с неё пропадает.
//
// Живёт здесь, а не внутри GoblinSystem, по той же причине, что и
// depositHumus (core/Humus.hpp) с depositCarcass (core/Carcass.hpp), на
// которые этот закон и написан по образцу: класть в кучу будет один, есть из
// неё другой, гноить третий, и ответ на вопрос "что такое куча" обязан быть
// у всех троих один.

// Сколько еды пропадает из кучи за тик.
//
// Гниёт вдесятеро медленнее туши (kCarcassRot = 20, core/Carcass.hpp), и это
// не поблажка, а разница между мясом и ягодой: ягоды в куче сохнут. Полная
// горсть держится пару тысяч тиков — достаточно, чтобы запас пережил
// неудачный день, и мало, чтобы пережить поколение.
//
// Гнить куча обязана, и причина та же, по которой гниёт падаль: иначе
// однажды набранный запас кормит вечно. Лагерь, накопивший гору за удачное
// лето, перестал бы зависеть от мира вовсе — а вместе с этим исчезла бы и
// причина куда-либо ходить.
constexpr int kStoreRot = 2;

// С какой кучи "нехватка крыши" считается полной. Полная горсть взрослого
// (kCarryPerSize, core/Carry.hpp): один раз принесённое ещё не повод строить,
// а вот запас, ради которого ходили не раз, укрыть уже стоит.
constexpr int kStoreShelterFull = 2000;

// Часто ли куче портиться. Постройки её берегут: под навесом вдвое, на
// подстилке ещё вдвое, вместе вчетверо (core/Build.hpp) — и склад получается
// сам собой, стоит поставить навес над кучей. Отдельной постройки для него не
// заводится.
//
// Считается долей ТИКОВ, а не долей порции, и это не придирка. Порция гниения
// — две единицы; поделить её вчетверо целыми числами нельзя, первое же
// деление обратило бы её в ноль, и запас под навесом стал бы вечным. Доля
// тиков делится сколько угодно: гниёт не всегда, а столько раз из тысячи, во
// сколько куче мешают. Заодно это даёт постепенность — полунавес бережёт
// вполовину, как ему и положено (BuildingComponent).
//
// Сдвиг по номеру клетки — чтобы кучи всего мира не теряли по горсти в один и
// тот же тик; тот же приём, что у ягод, троп и ветшания.
inline bool storeRotDue(std::uint64_t tick, std::size_t cell, int canopy, int bedding) {
    const int protection = (kFull + std::clamp(canopy, 0, kFull)) *
                            (kFull + std::clamp(bedding, 0, kFull)) / kFull;
    const int share = kFull * kFull / std::max(kFull, protection);
    return (tick + static_cast<std::uint64_t>(cell)) % static_cast<std::uint64_t>(kFull) <
            static_cast<std::uint64_t>(share);
}

// Положить еду на клетку. Если куча там уже есть, положенное складывается с
// ней: две горсти на одном тайле — это одна куча, а не две.
//
// Вызывать только из команды очереди (05_Entity.md, п.5): добавление
// компонента — структурное изменение, и во время обхода систем его быть не
// должно.
inline void depositStore(World& world, int x, int y, int food, int minerals) {
    if ((food <= 0 && minerals <= 0) || !world.area().inBounds(x, y)) {
        return;
    }
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!world.registry().all_of<SoilComponent>(tile)) {
            continue;
        }
        if (auto* store = world.registry().try_get<StoreComponent>(tile)) {
            store->food += food;
            store->minerals += minerals;
        } else {
            world.registry().emplace<StoreComponent>(tile, StoreComponent{food, minerals});
        }
        return;
    }
}

// Взять из кучи — съесть. Крупицы уходят той же долей (core/Portion.hpp).
inline Portion takeFromStore(StoreComponent& store, int want) {
    return takePortion(store.food, store.minerals, want);
}

} // namespace goblins

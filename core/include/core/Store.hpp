#pragma once

#include "core/Portion.hpp"
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

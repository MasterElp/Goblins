#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/Portion.hpp"
#include "core/Resources.hpp"
#include "core/Scale.hpp"
#include "core/World.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/StoreComponent.hpp"

namespace goblins {

// Куча: как принесённое ложится на землю, сколько её туда влезает и как оно с
// неё пропадает.
//
// Живёт здесь, а не внутри GoblinSystem, по той же причине, что и
// depositHumus (core/Humus.hpp) с depositCarcass (core/Carcass.hpp), на
// которые этот закон и написан по образцу: класть в кучу будет один, есть из
// неё другой, строить из неё третий, гноить четвёртый — и ответ на вопрос
// "что такое куча" обязан быть у всех один.

// Сколько всего вмещает клетка — общим счётом на все виды ресурсов
// (core/Resources.hpp). Две полные горсти взрослого (kCarryPerSize): столько
// можно свалить на голую землю, и ни крошкой больше.
//
// Предел постоянный и построек не спрашивает. Полки, бочки и всё, что поднимет
// вместимость, придут отдельной постройкой — тогда это число станет основанием,
// а не потолком.
constexpr int kStoreCapacity = 4000;

// Сколько ресурса пропадает за один "гнилой" тик.
constexpr int kStoreRot = 2;

// С какой кучи еды "нехватка крыши" считается полной. Полная горсть взрослого:
// один раз принесённое ещё не повод строить, а вот запас, ради которого ходили
// не раз, укрыть уже стоит.
constexpr int kStoreShelterFull = 2000;

// Во сколько раз реже портится каждый вид. Еда гниёт как есть; солома
// вчетверо реже; ветки вдесятеро — им сохнуть, а не гнить.
//
// Порядок тот же, что у ResourceKind: список ведёт за собой и это число, и
// новый ресурс дописывается сюда строкой, а не заводит своей ветки в системе.
inline constexpr int kResourceKeeps[kResourceKinds] = {1, 4, 10};

// Сколько ещё влезет в эту кучу.
inline int storeRoom(const StoreComponent& store) {
    return std::max(0, kStoreCapacity - store.stored.total());
}

// Часто ли этому виду портиться на этой клетке.
//
// Постройки берегут запас: под навесом вдвое, на подстилке ещё вдвое, вместе
// вчетверо (core/Build.hpp) — и склад получается сам собой, стоит поставить
// навес над кучей. Отдельной постройки для него не заводится.
//
// Считается долей ТИКОВ, а не долей порции, и это не придирка. Порция гниения —
// две единицы; поделить её вчетверо целыми числами нельзя, первое же деление
// обратило бы её в ноль, и запас под навесом стал бы вечным. Доля тиков делится
// сколько угодно: гниёт не всегда, а столько раз из тысячи, во сколько куче
// мешают. Заодно это даёт постепенность — полунавес бережёт вполовину, как ему
// и положено (BuildingComponent).
//
// Стойкость вида растягивает то же окно: солома портится вчетверо реже еды,
// потому что её окно вчетверо длиннее.
//
// Сдвиг по номеру клетки — чтобы кучи всего мира не теряли по горсти в один и
// тот же тик; тот же приём, что у ягод, троп и ветшания.
inline bool storeRotDue(std::uint64_t tick, std::size_t cell, ResourceKind kind, int canopy, int bedding) {
    const int protection = (kFull + std::clamp(canopy, 0, kFull)) *
                            (kFull + std::clamp(bedding, 0, kFull)) / kFull;
    const int share = kFull * kFull / std::max(kFull, protection);
    const auto window = static_cast<std::uint64_t>(kFull) *
                         static_cast<std::uint64_t>(kResourceKeeps[static_cast<std::size_t>(kind)]);
    return (tick + static_cast<std::uint64_t>(cell)) % window < static_cast<std::uint64_t>(share);
}

// Положить на клетку. Если куча там уже есть, положенное складывается с ней:
// две горсти на одном тайле — это одна куча, а не две.
//
// Больше вместимости не влезет, и не влезшее НЕ ПРОПАДАЕТ: сколько принято,
// столько и сказано в ответе, остальное остаётся у того, кто клал. Считать,
// сколько поместится, вызывающая сторона должна заранее (storeRoom по снимку),
// а этот ответ — последняя проверка на случай, если между решением и командой
// кто-то успел свалить своё.
//
// Вызывать только из команды очереди (05_Entity.md, п.5): добавление
// компонента — структурное изменение.
inline Portion depositStore(World& world, int x, int y, ResourceKind kind, Portion what) {
    Portion taken;
    if (what.amount <= 0 || !world.area().inBounds(x, y)) {
        return taken;
    }
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!world.registry().all_of<SoilComponent>(tile)) {
            continue;
        }
        if (auto* store = world.registry().try_get<StoreComponent>(tile)) {
            return addResource(store->stored, kind, what, storeRoom(*store));
        }
        StoreComponent fresh;
        taken = addResource(fresh.stored, kind, what, kStoreCapacity);
        world.registry().emplace<StoreComponent>(tile, fresh);
        return taken;
    }
    return taken;
}

// Взять из кучи — съесть или пустить в дело. Крупицы уходят той же долей
// (core/Portion.hpp).
inline Portion takeFromStore(StoreComponent& store, ResourceKind kind, int want) {
    return takeResource(store.stored, kind, want);
}

} // namespace goblins

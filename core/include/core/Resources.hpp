#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/Portion.hpp"

namespace goblins {

// Что вообще можно взять в руки и сложить на землю — ОДИН список на весь мир.
//
// До него еда жила своим полем в руках и своим в куче, а материал — своими
// полями на стройплощадке, и третий вид потребовал бы третьего поля в руках,
// третьего в куче, третьего в файле мира и третьего в протоколе. Список
// вместо полей стоит того: новый ресурс — это строка здесь и строка в имени,
// а всё остальное перебирает виды само.
//
// Правило простое и общее: **всё, что носят, можно и складывать**. Руки и
// куча держат одно и то же, отличаясь только вместимостью и тем, что куча не
// ходит.

enum class ResourceKind : std::uint8_t {
    Food = 0,   // ягоды, мясо, всё съедобное — одна шкала биомассы
    Straw = 1,  // срезанная трава
    Twigs = 2,  // наломанные ветки; втрое ценнее соломы в стройке
};

inline constexpr int kResourceKinds = 3;

// Имя вида — не логика, а имя значения (см. placeKindName, buildKindName): им
// пользуются файл мира и панель наблюдения, и лежит оно рядом с самим
// перечислением, чтобы не разъехаться.
inline const char* resourceName(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::Straw: return "straw";
        case ResourceKind::Twigs: return "twigs";
        case ResourceKind::Food: break;
    }
    return "food";
}

inline ResourceKind resourceFromName(const std::string& name) {
    if (name == "straw") return ResourceKind::Straw;
    if (name == "twigs") return ResourceKind::Twigs;
    return ResourceKind::Food;
}

// Сколько чего лежит (в руках, в куче — где угодно).
//
// Крупицы минералов — одним числом на всю связку, и означают они то же, что и
// всегда: вещество, приехавшее внутрь ЕДЫ. Материал крупиц не несёт — ветку
// ломают, а корни остаются в земле, и когда растение умрёт, они лягут
// перегноем целиком.
struct Resources {
    std::array<int, kResourceKinds> amount{};
    int minerals = 0;

    int of(ResourceKind kind) const { return amount[static_cast<std::size_t>(kind)]; }
    int& of(ResourceKind kind) { return amount[static_cast<std::size_t>(kind)]; }

    // Сколько всего занято. Вместимость и у рук, и у клетки общая на все
    // виды: набравший полные руки веток не унесёт ничего съестного, а клетка
    // вмещает столько-то, чем бы оно ни было.
    int total() const {
        int sum = 0;
        for (const int value : amount) {
            sum += value;
        }
        return sum;
    }
};

// Взять часть одного вида. Крупицы уходят долей только с едой — больше их
// нигде и нет (core/Portion.hpp).
inline Portion takeResource(Resources& res, ResourceKind kind, int want) {
    if (kind == ResourceKind::Food) {
        return takePortion(res.of(ResourceKind::Food), res.minerals, want);
    }
    Portion taken;
    taken.amount = std::max(0, std::min(want, res.of(kind)));
    res.of(kind) -= taken.amount;
    return taken;
}

// Положить, но не больше room. Возвращает принятое — остальное остаётся у
// того, кто клал: вещество в этом мире не исчезает оттого, что ему не хватило
// места.
inline Portion addResource(Resources& res, ResourceKind kind, Portion what, int room) {
    Portion taken;
    if (what.amount <= 0 || room <= 0) {
        return taken;
    }
    taken.amount = std::min(what.amount, room);
    // Крупицы едут долей от принятого: приняли половину — приехала половина.
    taken.minerals = what.amount > 0 ? what.minerals * taken.amount / what.amount : 0;
    res.of(kind) += taken.amount;
    res.minerals += taken.minerals;
    return taken;
}

} // namespace goblins

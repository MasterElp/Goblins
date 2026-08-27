#pragma once

#include <cstdint>
#include <string>

namespace goblins {

// Что здесь решено построить.
enum class BuildKind : std::uint8_t {
    None = 0,
    Canopy = 1,
    Bedding = 2,
};

// Имя вида — не логика, а имя значения (см. placeKindName, sexName): им
// пользуются файл мира и сетевой протокол, и лежит оно рядом с самим
// перечислением, чтобы не разъехаться.
inline const char* buildKindName(BuildKind kind) {
    switch (kind) {
        case BuildKind::Canopy: return "canopy";
        case BuildKind::Bedding: return "bedding";
        case BuildKind::None: break;
    }
    return "none";
}

inline BuildKind buildKindFromName(const std::string& name) {
    if (name == "canopy") return BuildKind::Canopy;
    if (name == "bedding") return BuildKind::Bedding;
    return BuildKind::None;
}

// Стройплощадка — ЗАМЫСЕЛ, видимый со стороны.
//
// До неё всё, что делал один гоблин, было делом одного гоблина: он ел, пил,
// ложился, нёс. Площадку видно на карте и в снимке клеток, как всякое другое
// состояние земли, — и потому в неё может вложиться любой, кто мимо шёл.
// Это первое в мире общее дело (02_CorePrinciples.md, п.11: некоторая
// деятельность может требовать нескольких исполнителей).
//
// Ставит её тот, кому плохо на обжитом месте (см. GoblinSystem); снимается
// она сама, когда прочность построенного дошла до полной.
struct SiteComponent {
    BuildKind kind = BuildKind::None;

    // Материал, принесённый сюда, но ещё не пущенный в дело. Трава и ветки
    // порознь, потому что ветка стоит трёх соломин (core/Build.hpp): сложить
    // их в одно число значило бы потерять разницу в прочности, ради которой
    // за ветками и ходят.
    int straw = 0;
    int twigs = 0;

    // Неделящийся остаток работы, 0..стоимость-1. Единица труда даёт
    // kFull/стоимость прочности — величину, которая целой почти никогда не
    // выходит, и остаток дожидается здесь. Целый остаток, а не дробь
    // (core/Scale.hpp): тот же приём, что у шага животного (stepProgress).
    int progress = 0;
};

} // namespace goblins

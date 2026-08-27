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
// Ставит её тот, кому плохо на обжитом месте (см. GoblinSystem), а СНИМАЕТСЯ
// она первой же единицей работы: дальше на клетке стоит не план, а
// недостроенное здание, то есть постройка малой прочности
// (BuildingComponent). Замысел и недостроенное — разные вещи, и держать
// первое после начала второго незачем.
//
// Оттого в ней и не осталось ничего, кроме вида задуманного: материал лежит
// в общей куче на той же клетке (core/Store.hpp) или в руках работающего, а
// неделящегося остатка труда не бывает вовсе — цены построек выбраны так,
// чтобы прочность росла целыми числами (core/Work.hpp).
struct SiteComponent {
    BuildKind kind = BuildKind::None;
};

} // namespace goblins

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace goblins {

// Что гоблин помнит о мире — и первое, чего у животного нет вовсе
// (docs/09_Animals.md, п.16: памяти нет).
//
// Зверь ходит только туда, что видит сейчас: он не может хотеть того, о чём
// не знает (02_CorePrinciples.md, п.6). Пока это верно и для гоблина, его
// маршруты случайны — он каждый раз заново находит ближайшее подходящее и ту
// же клетку выбирает только по совпадению. Память — то, что делает путь
// ПОВТОРЯЮЩИМСЯ, а из повторяющегося пути и вырастает всё остальное: тропа,
// перекрёсток, место, к которому приходят все.
//
// Знание принадлежит Entity (02_CorePrinciples.md, п.6, п.7), и это не
// оговорка: общего "списка интересных мест мира" здесь нет и быть не должно.
// Два гоблина обязаны помнить разное — иначе поселение возникнет не оттого,
// что они сошлись, а оттого, что им раздали один справочник.

enum class PlaceKind : std::uint8_t {
    None = 0,
    Food = 1,
    Water = 2,
    Rest = 3,
};

inline const char* placeKindName(PlaceKind kind) {
    switch (kind) {
        case PlaceKind::Food: return "food";
        case PlaceKind::Water: return "water";
        case PlaceKind::Rest: return "rest";
        case PlaceKind::None: break;
    }
    return "none";
}

inline PlaceKind placeKindFromName(const std::string& name) {
    if (name == "food") return PlaceKind::Food;
    if (name == "water") return PlaceKind::Water;
    if (name == "rest") return PlaceKind::Rest;
    return PlaceKind::None;
}

// Одно запомненное место: где, чем оно было хорошо и насколько твёрдо это
// помнится.
struct KnownPlace {
    int x = 0;
    int y = 0;
    PlaceKind kind = PlaceKind::None;
    // 0..kFull. Растёт от того, что место пригодилось, убывает само со
    // временем и резко — от того, что гоблин пришёл и ничего не нашёл.
    int strength = 0;
};

// Сколько мест помнит гоблин. Ровно столько, и это часть закона, а не
// экономия памяти: голова не растёт.
//
// Из ограниченности и берётся всё интересное. Помни гоблин всё, он ходил бы
// по идеальному маршруту и никогда ничему не удивлялся; помня восемь мест,
// он вынужден ВЫБИРАТЬ, что держать в голове, — и вытесненное забывает
// по-настоящему. Число небольшое нарочно: столько мест хватает на воду,
// пару кормовых участков и место, где спится, то есть ровно на то, из чего
// складывается день.
inline constexpr int kKnownPlaces = 8;

struct KnowledgeComponent {
    std::array<KnownPlace, kKnownPlaces> places{};
};

} // namespace goblins

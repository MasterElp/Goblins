#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "core/Path.hpp"
#include "core/components/AnimalComponent.hpp"

namespace goblins {

// К кому пойдёт зверь, которому нужна пара, — тот же закон и на систему, и
// на наблюдателя, что и охота (core/Hunting.hpp).
//
// Пару ищут дорогой по той же причине, по какой хищник дорогой ищет добычу:
// увиденное через реку — это ещё не найденное. Пара за водой видна обоим,
// сойтись им негде, и оба стоят: самка ждёт на месте, самец идёт напролом и
// упирается в берег. Так и проходит остаток их жизни — в двадцати шагах
// друг от друга, каждый со своей стороны воды.
//
// Здесь только выбор. Сама встреча (кто кого дождался, кто с кем сошёлся на
// одной клетке и что из этого вышло) остаётся в AnimalSystem: это уже не
// знание зверя, а событие мира.

// Сам жених или невеста: где стоит, докуда видит, кто он такой.
struct Suitor {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    int perception = 1;
    int species = 0;
    bool predator = false;
    Sex sex = Sex::Female;
};

// Возможная пара — тем, что о ней видно со стороны. "willing" — согласен ли
// он сам: занятый едой или бегущий от хищника не сойдётся ни с кем, и
// умерший в этот тик тоже.
struct MateCandidate {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    int species = 0;
    bool predator = false;
    Sex sex = Sex::Female;
    bool willing = false;
};

struct MateChoice {
    bool found = false;
    int x = 0;
    int y = 0;
};

// Пара — своего вида, своей диеты, другого пола и согласная. Вид и диета
// проверяются, а не подразумеваются: на одной поляне пасутся несколько
// видов, и от чужого потомства не бывает.
inline bool mateSuits(const Suitor& suitor, const MateCandidate& candidate) {
    return candidate.willing && candidate.id != suitor.id && candidate.predator == suitor.predator &&
           candidate.species == suitor.species && candidate.sex != suitor.sex;
}

// Есть ли вообще на кого смотреть. Отдельно от выбора, потому что перебор
// десятков животных дёшев, а волна по округе — нет: без этой проверки
// каждый ищущий пару зверь считал бы дорогу до всей своей округи каждый
// тик, а ищут её многие и подолгу.
inline bool anyMateInSight(const Suitor& suitor, std::span<const MateCandidate> candidates) {
    const int sight = std::max(1, suitor.perception);
    for (const auto& candidate : candidates) {
        if (!mateSuits(suitor, candidate)) {
            continue;
        }
        const int dx = candidate.x - suitor.x;
        const int dy = candidate.y - suitor.y;
        if (dx * dx + dy * dy <= sight * sight) {
            return true;
        }
    }
    return false;
}

// К кому идти: ближайшая по дороге пара. Ближе — это меньше шагов, а не
// короче отрезок: как и у хищника, для идущего ногами близость меряется
// дорогой, и пара за излучиной реки близка глазу и далека ногам.
//
// Из равно далёких берётся та, у которой меньше идентификатор, а не та, что
// раньше в списке: порядок в памяти не может быть причиной события в мире
// (02_CorePrinciples.md, п.12a), а имя в мире у каждого своё.
inline MateChoice chooseMate(const Reach& reach, const Suitor& suitor,
                             std::span<const MateCandidate> candidates) {
    const int sight = std::max(1, suitor.perception);
    MateChoice choice;
    int bestDistance = 0;
    std::uint64_t bestId = 0;
    for (const auto& candidate : candidates) {
        if (!mateSuits(suitor, candidate)) {
            continue;
        }
        const int dx = candidate.x - suitor.x;
        const int dy = candidate.y - suitor.y;
        if (dx * dx + dy * dy > sight * sight) {
            continue;
        }
        // Своя клетка дороги не требует: пара, стоящая тут же, уже найдена
        // — с неё и начинается встреча.
        const int distance = reach.steps(candidate.x, candidate.y);
        if (distance < 0) {
            continue; // дороги нет: увиденное через реку — ещё не найденное
        }
        if (choice.found && (distance > bestDistance || (distance == bestDistance && candidate.id > bestId))) {
            continue;
        }
        choice = MateChoice{true, candidate.x, candidate.y};
        bestDistance = distance;
        bestId = candidate.id;
    }
    return choice;
}

} // namespace goblins

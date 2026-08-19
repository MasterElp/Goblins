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
// А вот пару, которую не видно вовсе (она дальше радиуса восприятия), ищут
// не дорогой, а зовом (hearCall, ниже): самка, которой нужна пара, слышна
// дальше, чем видна, и самец идёт на голос, а не наугад. Разница
// принципиальная: дорога отвечает "дойду ли я, и как", зов — только "куда
// пробовать", а как дойти, решит уже сам шаг (core/Walk.hpp), огибая
// преграды тем же способом, каким их огибает слепое блуждание.
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

// Как далеко разносится зов пары — дальше, чем видно (perception), но не
// весь мир: это слух, а не всеведение, и предел ему нужен по той же
// причине, по которой предел есть у зрения (02_CorePrinciples.md, п.6).
// Одно число на все виды и обе диеты: не черта генома, а то, как в этом
// мире распространяется звук, — то же самое место, что занимает kFull для
// шкалы долей.
//
// Без зова разбросанное по большой карте поголовье вымирало не от голода и
// не от зубов, а от одиночества: чтобы принести потомство, двум последним
// зверям нужно ещё и встретиться, а слепое блуждание (core/Walk.hpp,
// roamDirection) сводит их вместе примерно никогда — каждый уходит в свою
// случайную сторону, и предпоследняя пара расходится дальше, а не ближе.
constexpr int kCallRange = 40;

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

// Зовёт ли этот кандидат. Зовёт только самка — тот же пол, что и ждёт на
// месте, дождавшись жениха (см. AnimalSystem, п.10, "Ждёт она..."): не
// потому, что самец молчалив, а потому, что зов самца был бы бесполезен —
// искать и идти на голос умеет только тот, кто вообще куда-то ходит за
// парой, а самка с места не сходит, что бы она ни услышала.
inline bool calls(const MateCandidate& candidate) {
    return candidate.willing && candidate.sex == Sex::Female;
}

// Направление на зов, когда рядом никого не видно совсем (сперва проверяют
// anyMateInSight — этот закон для того случая, когда он ответил "нет").
// Не дорога, а прямая цель: звук не спрашивает брода и слышен дальше, чем
// видно, поэтому здесь нет ни своего радиуса видимости, ни проверки
// core/Path.hpp — как до зовущей дойти, решит уже сам шаг
// (core/Walk.hpp), тем же способом, каким слепое блуждание само огибает
// преграды.
//
// Из нескольких зовущих на одинаковом расстоянии побеждает меньший
// идентификатор — по той же причине, что и в chooseMate: порядок в памяти
// не может быть причиной события в мире (02_CorePrinciples.md, п.12a).
inline MateChoice hearCall(const Suitor& suitor, std::span<const MateCandidate> candidates) {
    MateChoice choice;
    int bestDistance = 0;
    std::uint64_t bestId = 0;
    for (const auto& candidate : candidates) {
        if (!mateSuits(suitor, candidate) || !calls(candidate)) {
            continue;
        }
        const int dx = candidate.x - suitor.x;
        const int dy = candidate.y - suitor.y;
        const int distance = dx * dx + dy * dy;
        if (distance > kCallRange * kCallRange) {
            continue;
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

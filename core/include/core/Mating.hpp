#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "core/Bonds.hpp"
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
//
// "grown" — жив и вырос, то есть тот, к кому за парой ходят ВООБЩЕ, пусть
// сейчас он и занят другим. Отдельно от согласия, и это не мелочь: согласие
// живёт один тик и совпадает у двоих редко, а вырос человек надолго. На
// согласии строят встречу, на "вырос" — всё остальное: кого запомнить, куда
// вернуться, к кому подойти и подождать.
struct MateCandidate {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    int species = 0;
    bool predator = false;
    Sex sex = Sex::Female;
    bool willing = false;
    bool grown = false;
};

struct MateChoice {
    bool found = false;
    // Кто именно — а не только куда идти. Нужно тому, кто помнит лица
    // (core/Bonds.hpp): встреча с близким запоминается крепче случайной, и
    // без имени пары эту разницу не на чем построить. Зверю имя не нужно и
    // не мешает: он его просто не спрашивает.
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
};

// Во что обходится каждый шаг до пары, когда идущий помнит лица.
//
// Шестьдесят означает, что полная симпатия (kFull) стоит шестнадцати шагов —
// примерно всего поля зрения гоблина (зоркость 5..18). То есть за своей
// пойдут через всю округу мимо чужой, стоящей рядом, но не дальше, чем
// видно: симпатия перевешивает расстояние, а не отменяет его.
//
// Меньше — и симпатия решала бы только при равном расстоянии, то есть почти
// никогда. Больше — и ближняя пара не выигрывала бы вовсе, а гоблин ходил бы
// через полкарты к единственному тёплому лицу, минуя всех.
constexpr int kMateWarmthStep = 60;

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

// Ровня: своего вида, своей диеты, другого пола и выросший. Вид и диета
// проверяются, а не подразумеваются: на одной поляне пасутся несколько
// видов, и от чужого потомства не бывает.
//
// Согласия здесь нет, и в этом весь смысл разделения: ровню видно и тогда,
// когда она занята. Тому, кто помнит места и лица, этого достаточно —
// запомнить, где ходят такие, можно и не сходясь с ними (см. sightOfMate).
inline bool mateKind(const Suitor& suitor, const MateCandidate& candidate) {
    return candidate.grown && candidate.id != suitor.id && candidate.predator == suitor.predator &&
           candidate.species == suitor.species && candidate.sex != suitor.sex;
}

// Пара — та же ровня, но согласная сейчас. Сходятся только с такой.
inline bool mateSuits(const Suitor& suitor, const MateCandidate& candidate) {
    return candidate.willing && mateKind(suitor, candidate);
}

// Есть ли вообще на кого смотреть. Отдельно от выбора, потому что перебор
// десятков животных дёшев, а волна по округе — нет: без этой проверки
// каждый ищущий пару зверь считал бы дорогу до всей своей округи каждый
// тик, а ищут её многие и подолгу.
//
// Спросить можно двумя способами, и это те же два вопроса, что и везде ниже:
// willingOnly — "есть ли с кем сойтись сейчас", без него — "есть ли тут
// вообще такие". Второй вопрос нужен тому, кто умеет ждать и помнить.
inline bool anyMateInSight(const Suitor& suitor, std::span<const MateCandidate> candidates,
                           bool willingOnly = true) {
    const int sight = std::max(1, suitor.perception);
    for (const auto& candidate : candidates) {
        if (willingOnly ? !mateSuits(suitor, candidate) : !mateKind(suitor, candidate)) {
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
        choice = MateChoice{true, candidate.id, candidate.x, candidate.y};
        bestDistance = distance;
        bestId = candidate.id;
    }
    return choice;
}

// Кого из ровни видно ближе всех — без дороги и без согласия.
//
// Это не выбор, к кому идти, а ответ на вопрос "кто тут ходит": им
// пользуется память места (PlaceKind::Mate), которой всё равно, согласен ли
// человек сейчас и как до него добираться. Помнят не человека, а поляну, на
// которой он попался, — а поляна не обманет и завтра.
//
// Дороги здесь поэтому нет намеренно: волна стоит дорого, а спрашивают это
// каждый тик и все. Ошибиться она не даст ничему: по вспомненному месту
// гоблин пойдёт всё равно вслепую, памятью ног.
//
// Симпатия сокращает расстояние тем же порядком, что и при выборе пары
// (kMateWarmthStep): место, где видели своих, ляжет в голову раньше места,
// где видели чужих. Шаги — восемью соседями (как в recall,
// core/Knowledge.hpp), а поле зрения кругом: одно про то, куда идти, другое
// про то, докуда видно.
inline MateChoice sightOfMate(const Suitor& suitor, std::span<const MateCandidate> candidates,
                              const BondsComponent* bonds = nullptr) {
    const int sight = std::max(1, suitor.perception);
    MateChoice choice;
    int bestDistance = 0;
    std::uint64_t bestId = 0;
    for (const auto& candidate : candidates) {
        if (!mateKind(suitor, candidate)) {
            continue;
        }
        const int dx = candidate.x - suitor.x;
        const int dy = candidate.y - suitor.y;
        if (dx * dx + dy * dy > sight * sight) {
            continue;
        }
        const int steps = std::max(std::abs(dx), std::abs(dy));
        const int warmth = bonds != nullptr ? warmthTo(*bonds, candidate.id) : 0;
        const int distance = steps * kMateWarmthStep - warmth;
        if (choice.found && (distance > bestDistance || (distance == bestDistance && candidate.id > bestId))) {
            continue;
        }
        choice = MateChoice{true, candidate.id, candidate.x, candidate.y};
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
//
// Симпатия при этом СОКРАЩАЕТ дорогу: тот, к кому тепло, стоит ближе, чем
// стоит на самом деле (kMateWarmthStep). Знакомство сюда попадает то же
// самое, по которому гоблин выбирает собеседника (core/Bonds.hpp), и это не
// натяжка, а всё, чем симпатия в этом мире и является: тепло копится от
// встреч, а не заводится отдельным чувством к паре.
//
// Лиц может не быть вовсе (bonds == nullptr) — так ищет пару зверь. Он
// выбирает ближайшую и ничего не теряет: помнить ему нечем
// (docs/09_Animals.md, п.16).
//
// willingOnly — тот же второй вопрос, что и у anyMateInSight: без него идут
// не сходиться, а ПОДОЙТИ И ЖДАТЬ рядом с тем, кто сейчас занят. Дорога
// нужна и здесь: ждать через реку значит ждать зря.
inline MateChoice chooseMate(const Reach& reach, const Suitor& suitor,
                             std::span<const MateCandidate> candidates,
                             const BondsComponent* bonds = nullptr, bool willingOnly = true) {
    const int sight = std::max(1, suitor.perception);
    MateChoice choice;
    int bestDistance = 0;
    std::uint64_t bestId = 0;
    for (const auto& candidate : candidates) {
        if (willingOnly ? !mateSuits(suitor, candidate) : !mateKind(suitor, candidate)) {
            continue;
        }
        const int dx = candidate.x - suitor.x;
        const int dy = candidate.y - suitor.y;
        if (dx * dx + dy * dy > sight * sight) {
            continue;
        }
        // Своя клетка дороги не требует: пара, стоящая тут же, уже найдена
        // — с неё и начинается встреча.
        const int steps = reach.steps(candidate.x, candidate.y);
        if (steps < 0) {
            continue; // дороги нет: увиденное через реку — ещё не найденное
        }
        const int warmth = bonds != nullptr ? warmthTo(*bonds, candidate.id) : 0;
        const int distance = steps * kMateWarmthStep - warmth;
        if (choice.found && (distance > bestDistance || (distance == bestDistance && candidate.id > bestId))) {
            continue;
        }
        choice = MateChoice{true, candidate.id, candidate.x, candidate.y};
        bestDistance = distance;
        bestId = candidate.id;
    }
    return choice;
}

} // namespace goblins

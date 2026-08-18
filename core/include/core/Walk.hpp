#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "core/Random.hpp"
#include "core/Scale.hpp"
#include "core/components/MovementComponent.hpp"

namespace goblins {

// Один шаг: куда животное ставит ногу, когда уже решило, в какую сторону
// ему надо.
//
// Это не поиск пути (core/Path.hpp) и не замена ему. Дорога отвечает на
// вопрос "дойду ли я туда вообще и как", и спрашивают её только те, у кого
// есть кого искать, — хищник и жених. Шаг же делают все и каждый тик, в том
// числе те, кто идёт наугад: за травой, к воде, прочь от зубов, в
// выбранную сторону поиска. Поэтому здесь — только ближайшие восемь клеток
// и то, что животное помнит ногами (MovementComponent).
//
// Зачем вообще что-то помнить. Пока шаг выбирался одной лишь близостью к
// цели, зверь ходил как решётка, а не как зверь: упёршись в берег, он
// каждый тик заново выбирал ту же цель, шагал вбок, возвращался и снова
// шагал вбок — A → B → A → B до самой смерти; в углу между камнем и водой
// он топтался вечно; стадо на ровном лугу двигалось строем. Всё это лечится
// не поиском пути, а памятью на несколько шагов и щепотью случайности.
//
// Складывается шаг из пяти слагаемых, и все они — очки, а не запреты:
// запрет разом отнимает у зверя все клетки, а очки лишь меняют, какая из
// них лучше.
//   1. Тяга к цели: прямо на неё — полные очки, вбок — половина, поперёк —
//      ноль, назад — минус. Никакого "шаг обязан приближать": именно это
//      требование и заставляло зверя стоять перед берегом.
//   2. Инерция: продолжать начатое чуть выгоднее, чем поворачивать. Малая
//      величина против тяги к цели — она не ведёт, а только сглаживает.
//   3. След: клетки, с которых животное только что пришло, дороже свежих.
//      Отсюда и обход преграды, и невозможность вечного A → B → A → B.
//   4. Память о преградах: направление, которым только что не удалось
//      пройти, ненадолго теряет в цене — чтобы зверь не бился в стену раз
//      за разом, пока стена не отойдёт сама.
//   5. Случайность: небольшая всегда, и заметно больше — когда животное
//      застряло. Ровно она и вытаскивает из угла, где все разумные
//      направления одинаково плохи.

// Восемь направлений ПО КРУГУ: соседние отличаются на осьмушку оборота.
// Порядок здесь важен, в отличие от обхода соседей в других местах: по
// разнице номеров считается, насколько один поворот отличается от другого
// (walkTurns), а на порядке "слева направо, сверху вниз" такая разница
// ничего не значила бы.
inline constexpr int kWalkX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
inline constexpr int kWalkY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// Направление на клетку; -1, если это клетка под ногами.
inline int walkDirectionTo(int fromX, int fromY, int toX, int toY) {
    const int dx = (toX > fromX) - (toX < fromX);
    const int dy = (toY > fromY) - (toY < fromY);
    for (int dir = 0; dir < 8; ++dir) {
        if (kWalkX[dir] == dx && kWalkY[dir] == dy) {
            return dir;
        }
    }
    return -1;
}

// Насколько два направления расходятся: 0 — то же самое, 2 — поперёк, 4 —
// ровно назад. Круг замкнут, поэтому берётся короткая сторона.
inline int walkTurns(int a, int b) {
    const int difference = std::abs(a - b);
    return std::min(difference, 8 - difference);
}

// --- Веса шага ---
// Все в очках; цена полного оборота видна из того, как они соотносятся.

// Тяга к цели. Прямо — kAimPull, вбок (осьмушка) — половина, поперёк —
// ноль, назад — минус kAimPull.
constexpr int kAimPull = 1000;

// Инерция. Вшестеро слабее тяги: она сглаживает походку, а не ведёт её.
// Больше — и животное проносилось бы мимо поворота к самой еде.
constexpr int kInertiaPull = 150;

// Цена возврата на свой след — за самую свежую клетку; чем дальше шаг в
// памяти, тем она меньше. Подобрана так, чтобы шаг назад проигрывал шагу
// вбок, но выигрывал у шага в никуда: возвращаться зверь должен уметь,
// просто не в первую очередь.
constexpr int kTrailPenalty = 600;

// Цена направления, которым только что не прошли.
constexpr int kBlockedPenalty = 500;
// Насколько эта память тает за тик: преграда забывается примерно за
// десяток тиков.
constexpr int kBlockedFade = 100;

// Случайность шага: всегда немного, и до kStepNoise + kStuckNoise у
// застрявшего. У застрявшего она и должна перебивать всё остальное — иначе
// из угла не выйти: там все разумные направления одинаково плохи, и
// разумностью выбор не сделать.
constexpr int kStepNoise = 200;
constexpr int kStuckNoise = 900;

// Как быстро копится и убывает застревание. Четыре неудачных шага доводят
// его до предела, восемь удачных — гасят: выбираться из угла зверь должен
// заметно быстрее, чем успокаиваться.
constexpr int kStuckGain = 250;
constexpr int kStuckRelief = 120;

// Куда животное шагнуло. moved == false — шагнуть некуда вовсе: вокруг
// вода, камень или край мира.
struct WalkStep {
    bool moved = false;
    int direction = -1;
    int x = 0;
    int y = 0;
};

// Память о преградах тает сама собой, а не от шагов: вода уходит, соседи
// расходятся, и стена, в которую животное упёрлось, могла перестать быть
// стеной, пока оно стояло и ело. Застревание так не тает — его гасит только
// сдвинувшийся зверь (см. chooseStep): пока он топчется, ничего не
// изменилось.
inline void fadeWalkMemory(MovementComponent& memory) {
    for (int dir = 0; dir < 8; ++dir) {
        memory.blocked[dir] = std::max(0, memory.blocked[dir] - kBlockedFade);
    }
}

// Шаг из клетки (x, y) в сторону aimDirection (0..7; -1 — животному всё
// равно, куда идти). standable(x, y) — годна ли клетка под ногу
// (core/Path.hpp). Память шага при этом обновляется: она и есть то, чем
// животное отличает "иду" от "топчусь".
template <typename Standable>
WalkStep chooseStep(MovementComponent& memory, int x, int y, int aimDirection, Standable&& standable,
                    std::uint64_t& random) {
    // Клетки за спиной. Хранятся направлениями, поэтому восстанавливаются
    // шагами назад от нынешней клетки — по одному шагу на запись.
    int trailX[kTrailSteps] = {};
    int trailY[kTrailSteps] = {};
    int trail = 0;
    {
        int backX = x;
        int backY = y;
        for (int k = 0; k < kTrailSteps; ++k) {
            const int direction = memory.recent[k];
            if (direction < 0) {
                break;
            }
            backX -= kWalkX[direction];
            backY -= kWalkY[direction];
            trailX[trail] = backX;
            trailY[trail] = backY;
            ++trail;
        }
    }

    const int panic = std::clamp(memory.stuck, 0, kFull);
    const int noise = kStepNoise + kStuckNoise * panic / kFull;
    const int lastDirection = memory.recent[0];

    WalkStep step;
    int bestScore = 0;
    int bestTrail = -1;
    bool bumped = false;
    for (int dir = 0; dir < 8; ++dir) {
        const int nx = x + kWalkX[dir];
        const int ny = y + kWalkY[dir];
        if (!standable(nx, ny)) {
            // Столкновение — это попытка пройти именно туда, куда надо, а
            // не всякая стена вокруг. Стены и так видно, а помнить стоит
            // то, что уже не вышло: иначе зверь бился бы в берег каждый
            // тик, пока голод гонит его на ту сторону.
            if (dir == aimDirection) {
                memory.blocked[dir] = kFull;
                bumped = true;
            }
            continue;
        }

        int score = static_cast<int>(randomBelow(random, static_cast<std::uint64_t>(noise)));
        if (aimDirection >= 0) {
            score += kAimPull * (2 - walkTurns(dir, aimDirection)) / 2;
        }
        if (lastDirection >= 0) {
            score += kInertiaPull * (2 - walkTurns(dir, lastDirection)) / 2;
        }
        score -= kBlockedPenalty * memory.blocked[dir] / kFull;

        // След: чем свежее клетка, тем дороже на неё возвращаться, а у
        // застрявшего цена возврата ещё и удваивается — именно это и
        // разрывает хождение туда-сюда между двумя клетками.
        int steppedBack = -1;
        for (int k = 0; k < trail; ++k) {
            if (trailX[k] == nx && trailY[k] == ny) {
                steppedBack = k;
                break;
            }
        }
        if (steppedBack >= 0) {
            const int freshness = kTrailSteps - steppedBack;
            score -= kTrailPenalty * freshness / kTrailSteps * (kFull + panic) / kFull;
        }

        if (step.moved && score <= bestScore) {
            continue;
        }
        step = WalkStep{true, dir, nx, ny};
        bestScore = score;
        bestTrail = steppedBack;
    }

    if (!step.moved) {
        // Шагнуть некуда вовсе: паводок отрезал клетку, зверь стоит в углу
        // между камнем и водой. Это самое застревание и есть.
        memory.stuck = std::min(kFull, memory.stuck + kStuckGain);
        return step;
    }

    // Продвижением считается шаг на свежую клетку, которым ни во что не
    // упёрлись. Шаг по своим же следам продвижением не считается, даже если
    // он был лучшим из возможных: именно так выглядит хождение по кругу.
    if (bumped || bestTrail >= 0) {
        memory.stuck = std::min(kFull, memory.stuck + kStuckGain);
    } else {
        memory.stuck = std::max(0, memory.stuck - kStuckRelief);
    }

    for (int k = kTrailSteps - 1; k > 0; --k) {
        memory.recent[k] = memory.recent[k - 1];
    }
    memory.recent[0] = step.direction;
    return step;
}

} // namespace goblins

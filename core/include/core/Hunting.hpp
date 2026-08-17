#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "core/Area.hpp"
#include "core/Random.hpp"

namespace goblins {

// Охота — ОДИН закон мира на систему и на наблюдателя, по той же причине,
// что голод и жажда в core/Needs.hpp: пользуются им уже двое — AnimalSystem
// (чтобы хищник выбрал, за кем идти) и сервер (чтобы показать эту дорогу на
// карте). Две копии одного закона разъезжаются молча, а нарисованная
// дорога, по которой зверь на самом деле не идёт, хуже ненарисованной:
// именно по ней и судят, работает ли охота вообще.
//
// Здесь только знание хищника: куда он может дойти, за кем пойдёт и какой
// дорогой. Удар, дележ туши и сама ходьба остаются в AnimalSystem — это уже
// не знание, а события мира.

// С какого голода хищник выходит на охоту. Порог выше общего kDesireFloor
// намеренно: слегка проголодавшийся зверь подберёт падаль, если она рядом,
// но гнаться за живой добычей не станет. Сытый хищник, пропускающий добычу
// мимо, — не поблажка стаду, а единственное, что вообще удерживает его
// численность: пока хищники убивали при любом голоде, они выбивали стадо
// подчистую и вымирали следом, сколько ни крути прочие числа.
constexpr int kHuntHunger = 350;

// Дотянуться зубами можно до своей клетки и до соседней. Не только до
// своей: жертва убегает каждый тик, и хищнику, который обязан встать ровно
// на её клетку, доставалась бы она только по случайности — охота
// выродилась бы в лотерею.
constexpr int kAttackReach = 1;

// Ниже этого мяса глодать нечего: хищник просто не считает такую тушу едой,
// иначе он сидел бы у обглоданных костей вместо охоты.
constexpr int kMinBiteMeat = 50;

// Куда животное может поставить ногу: не на занятый непроходимым объектом
// тайл, не мимо Области и не в воду. Вода — стена: животное не плавает и
// брода не знает, поэтому клетка, где вода есть вообще, непроходима для
// него так же, как булыжник. Река делит карту, а не замедляет ход.
//
// Правило одно, а спрашивают его по-разному: система тика — по снимку
// клеток, снятому один раз на весь тик, наблюдатель — по самому миру,
// клетка за клеткой, и первое поголовье — при расселении
// (AnimalSeeding.cpp: в воду не ставим, животное туда и само не пойдёт).
// Поэтому сюда вынесено само правило, а факты, из которых оно складывается,
// каждый берёт своим способом.
inline bool standableAt(bool blocked, bool hasSoil, int waterDepth) {
    return !blocked && hasSoil && waterDepth <= 0;
}

// Клетка дороги. Отдельная мелочь, а не PositionComponent: дорога — не
// состояние мира, а то, что хищник видит перед собой в этот тик.
struct HuntCell {
    int x = 0;
    int y = 0;
};

// Округа хищника: за сколько шагов он доходит до каждой клетки, если идти
// ногами по проходимым клеткам, и куда он не дойдёт вовсе.
//
// Волна расходится от самого хищника и не выходит за круг восприятия:
// дальше видимости зверь дороги не знает, и обход длиной в полкарты — это
// уже не "вижу, как дойти", а карта в голове (02_CorePrinciples.md, п.6).
//
// Объект живёт у вызывающей стороны и переиспользуется: за тик волна
// пускается столько раз, сколько в мире голодных хищников, а массивы у неё
// на всю Область. Чтобы не обнулять их перед каждой волной, клетка
// помечается номером волны, а не признаком "посещена".
class HuntReach {
public:
    // standable(x, y) — годна ли клетка под ногу (см. standableAt).
    template <typename Standable>
    void build(const Area& area, int fromX, int fromY, int radius, Standable&& standable) {
        if (width_ != area.width() || height_ != area.height()) {
            width_ = area.width();
            height_ = area.height();
            const std::size_t cells = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
            visited_.assign(cells, 0);
            steps_.assign(cells, 0);
            wave_ = 0;
        }
        fromX_ = fromX;
        fromY_ = fromY;
        ++wave_;
        queue_.clear();
        if (!area.inBounds(fromX, fromY)) {
            return;
        }

        // Клетка под ногами годна всегда, даже если встать на неё уже
        // нельзя: паводок мог залить её, пока зверь на ней стоял, — и тогда
        // дорога у него начинается прямо из воды.
        const std::size_t start = cellOf(fromX, fromY);
        visited_[start] = wave_;
        steps_[start] = 0;
        queue_.push_back(start);
        for (std::size_t head = 0; head < queue_.size(); ++head) {
            const std::size_t cell = queue_[head];
            const int cx = static_cast<int>(cell % static_cast<std::size_t>(width_));
            const int cy = static_cast<int>(cell / static_cast<std::size_t>(width_));
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + kStepX[dir];
                const int ny = cy + kStepY[dir];
                if (!area.inBounds(nx, ny)) {
                    continue;
                }
                const int dx = nx - fromX;
                const int dy = ny - fromY;
                if (dx * dx + dy * dy > radius * radius) {
                    continue; // видимость круглая, а не квадратная
                }
                const std::size_t next = cellOf(nx, ny);
                if (visited_[next] == wave_ || !standable(nx, ny)) {
                    continue;
                }
                visited_[next] = wave_;
                steps_[next] = steps_[cell] + 1;
                queue_.push_back(next);
            }
        }
    }

    // Дошла ли волна до клетки, то есть есть ли туда вообще ход.
    bool reached(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_ && visited_[cellOf(x, y)] == wave_;
    }

    // Сколько шагов идти до клетки; -1, если дороги нет.
    int steps(int x, int y) const { return reached(x, y) ? steps_[cellOf(x, y)] : -1; }

    // Дорога до цели: клетки от первого шага до самой цели. Пусто, если
    // дороги нет или идти некуда (цель под ногами).
    //
    // Волна шла от хищника, поэтому дорога разматывается с конца: от цели —
    // к соседу, до которого на шаг меньше, и так до самого хищника. Из
    // одинаково коротких дорог берётся та, чей шаг ближе к хищнику по
    // прямой: так она не виляет там, где можно идти ровно. Полное равенство
    // решать нечем и незачем — оно бывает лишь на зеркальных клетках, и обе
    // дороги там одной длины.
    void roadTo(int targetX, int targetY, std::vector<HuntCell>& out) const {
        out.clear();
        if (!reached(targetX, targetY)) {
            return;
        }
        int cx = targetX;
        int cy = targetY;
        while (steps_[cellOf(cx, cy)] > 0) {
            out.push_back(HuntCell{cx, cy});
            const int steps = steps_[cellOf(cx, cy)];
            int backX = 0;
            int backY = 0;
            int bestScore = 0;
            bool found = false;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + kStepX[dir];
                const int ny = cy + kStepY[dir];
                if (!reached(nx, ny) || steps_[cellOf(nx, ny)] != steps - 1) {
                    continue;
                }
                const int dx = nx - fromX_;
                const int dy = ny - fromY_;
                const int score = dx * dx + dy * dy;
                if (found && score >= bestScore) {
                    continue;
                }
                found = true;
                bestScore = score;
                backX = nx;
                backY = ny;
            }
            if (!found) {
                break; // не бывает: волна пришла сюда откуда-то
            }
            cx = backX;
            cy = backY;
        }
        std::reverse(out.begin(), out.end());
    }

    // Вся округа, до которой есть ход, — для наблюдателя: по ней видно, что
    // хищник считает своим участком, а что чужим берегом.
    void reachedCells(std::vector<HuntCell>& out) const {
        out.clear();
        for (const auto cell : queue_) {
            out.push_back(HuntCell{static_cast<int>(cell % static_cast<std::size_t>(width_)),
                                    static_cast<int>(cell / static_cast<std::size_t>(width_))});
        }
    }

private:
    std::size_t cellOf(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }

    static constexpr int kStepX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr int kStepY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

    int width_ = 0;
    int height_ = 0;
    int fromX_ = 0;
    int fromY_ = 0;
    std::vector<std::uint32_t> visited_;
    std::vector<int> steps_;
    std::vector<std::size_t> queue_;
    std::uint32_t wave_ = 0;
};

// Добыча глазами хищника: где стоит и как быстро бежит. Больше он о ней
// ничего не знает — ни возраста, ни запаса сил (02_CorePrinciples.md, п.6).
struct HuntPrey {
    int x = 0;
    int y = 0;
    int speed = 0;
};

// Сам хищник: где стоит, докуда видит, как быстро бежит, насколько голоден.
struct Hunter {
    int x = 0;
    int y = 0;
    int perception = 1;
    int speed = 0;
    int hunger = 0;
};

// Что хищник выбрал: живую добычу, тушу или ничего.
struct HuntChoice {
    enum class Kind { None, Prey, Carcass };
    Kind kind = Kind::None;
    int prey = -1;        // номер в списке добычи (при Kind::Prey)
    int x = 0;
    int y = 0;
    bool atTeeth = false; // добыча уже в пределах укуса: бить, а не идти
};

// За кем пойдёт хищник. carcassAt(x, y) — сколько мяса лежит на клетке.
// random — розыгрыш для равных находок (см. ниже); передаётся копией,
// поэтому наблюдатель, собрав его тем же способом, что и система, получит
// тот же выбор.
template <typename CarcassAt>
HuntChoice chooseHuntTarget(const HuntReach& reach, const Hunter& hunter, std::span<const HuntPrey> prey,
                            CarcassAt&& carcassAt, std::uint64_t random) {
    const int sight = std::max(1, hunter.perception);
    HuntChoice choice;

    // --- Живая добыча ---
    // Ближе всех и первой: гнаться за той, что дальше, когда рядом стоит
    // эта, бессмысленно. Но только если хищник и вправду голоден.
    int preyIndex = -1;
    int preyDistance = 0;
    for (std::size_t b = 0; hunter.hunger >= kHuntHunger && b < prey.size(); ++b) {
        // За тем, кто быстрее, гнаться незачем: догнать его нельзя, а силы
        // уйдут. Скорость чужого бега — ровно то знание, которое у хищника
        // есть: он её видит, в отличие от чужого возраста или запаса сил.
        //
        // Без этого правила хищник выбирал ближайшую добычу и гнался за
        // ней, даже если она заведомо уходила; на безнадёжные погони уходила
        // вся энергия, и первое поголовье вымирало за тысячу тиков в мире,
        // полном добычи. Заодно у стада появляется смысл вкладывать бюджет
        // в скорость: быстрого не преследуют вовсе.
        if (prey[b].speed >= hunter.speed) {
            continue;
        }
        const int dx = prey[b].x - hunter.x;
        const int dy = prey[b].y - hunter.y;
        if (dx * dx + dy * dy > sight * sight) {
            continue;
        }

        // Здесь дорога и решает. За добычей, до которой дороги нет, хищник
        // не гонится вовсе: она для него не добыча, а вид. Прежде он брал
        // ближайшую по прямой, шёл к ней, упирался в воду, а на следующий
        // тик выбирал её же, одну и ту же, — и так до голодной смерти на
        // берегу, пока по его сторону реки паслось другое стадо.
        //
        // И "ближе" считается дорогой, а не отрезком: для идущего ногами
        // ближе тот, до кого меньше шагов. Добыча за излучиной реки близка
        // глазу и далека ногам, и гнаться надо не за ней.
        //
        // Соседняя клетка — исключение: до неё дороги не требуется, в неё
        // кусают. Иначе хищник упустил бы жертву, которую залило
        // разлившейся водой у него под носом, — зубы туда достают.
        const bool atTeeth = std::abs(dx) <= kAttackReach && std::abs(dy) <= kAttackReach;
        int distance = kAttackReach;
        if (!atTeeth) {
            distance = reach.steps(prey[b].x, prey[b].y);
            if (distance < 0) {
                continue;
            }
        }
        if (preyIndex >= 0 && distance >= preyDistance) {
            continue;
        }
        preyIndex = static_cast<int>(b);
        preyDistance = distance;
    }

    if (preyIndex >= 0) {
        const auto& target = prey[static_cast<std::size_t>(preyIndex)];
        choice = HuntChoice{HuntChoice::Kind::Prey, preyIndex, target.x, target.y,
                            std::abs(target.x - hunter.x) <= kAttackReach &&
                                std::abs(target.y - hunter.y) <= kAttackReach};
        if (choice.atTeeth) {
            return choice; // бить, а не идти: дорога такому уже не нужна
        }
    }

    // --- Падаль ---
    // Видимая падаль важнее видимой добычи: зачем гнаться, когда рядом
    // лежит мясо. Хищнику всё равно, кто убил ту тушу (см.
    // PredatorComponent).
    //
    // Правило не косметическое. Пока хищник шёл к тому, что ближе, он
    // бросал недоеденную тушу ради свежей добычи и убивал куда больше, чем
    // съедал: по карте лежали десятки почти нетронутых туш, а стадо тем
    // временем выбивалось под ноль — и следом вымирали сами хищники.
    // "Сначала доешь" превращает лишнее убийство в редкость, а не в
    // правило.
    //
    // Годной считается только та туша, до которой ведёт дорога, и мерится
    // она тоже дорогой: берег на той стороне реки проходим, а хода на него
    // нет. Без этой проверки хищник вставал у самой воды напротив
    // недосягаемой туши и голодал насмерть — в мире была еда, до которой он
    // не мог дойти, и он не умел этого понять.
    //
    // Из одинаково далёких туш выбор бросается жребием, а не достаётся
    // первой по обходу: обход идёт с левого верхнего угла квадрата
    // видимости, и без жребия хищники дружно уходили бы вверх и влево — не
    // потому, что там лучше, а потому, что цикл начинается оттуда.
    int carcassDistance = -1;
    int ties = 0;
    for (int dy = -sight; dy <= sight; ++dy) {
        for (int dx = -sight; dx <= sight; ++dx) {
            const int nx = hunter.x + dx;
            const int ny = hunter.y + dy;
            const int distance = reach.steps(nx, ny);
            if (distance < 0 || carcassAt(nx, ny) <= kMinBiteMeat) {
                continue;
            }
            if (ties > 0 && distance > carcassDistance) {
                continue;
            }
            if (ties > 0 && distance == carcassDistance) {
                ++ties;
                if (randomBelow(random, static_cast<std::uint64_t>(ties)) != 0) {
                    continue;
                }
            } else {
                ties = 1;
            }
            carcassDistance = distance;
            choice.kind = HuntChoice::Kind::Carcass;
            choice.prey = -1;
            choice.atTeeth = false;
            choice.x = nx;
            choice.y = ny;
        }
    }

    return choice;
}

} // namespace goblins

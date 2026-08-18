#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/Area.hpp"

namespace goblins {

// Дорога: куда животное может дойти отсюда ногами и за сколько шагов.
//
// Отдельно от того, КОГО по ней ищут, потому что ищут по ней разное. Хищник
// — добычу и падаль (core/Hunting.hpp), всякое животное — пару
// (core/Mating.hpp), а закон дороги у всех один: восемь соседей, вода и
// камень непроходимы, дальше видимости дороги нет. Две копии этого закона
// разъехались бы молча и в самом неприятном месте — там, где зверь стоит на
// берегу и не идёт никуда.

// Куда животное может поставить ногу: не на занятый непроходимым объектом
// тайл, не мимо Области и не в воду. Вода — стена: животное не плавает и
// брода не знает, поэтому клетка, где вода есть вообще, непроходима для
// него так же, как булыжник. Река делит карту, а не замедляет ход.
//
// Правило одно, а спрашивают его по-разному: система тика — по снимку
// клеток, снятому один раз на весь тик, наблюдатель — по самому миру,
// клетка за клеткой, и расселение первого поголовья — при посадке
// (AnimalSeeding.cpp: в воду не ставим, животное туда и само не пойдёт).
// Поэтому сюда вынесено само правило, а факты, из которых оно складывается,
// каждый берёт своим способом.
inline bool standableAt(bool blocked, bool hasSoil, int waterDepth) {
    return !blocked && hasSoil && waterDepth <= 0;
}

// Клетка дороги. Отдельная мелочь, а не PositionComponent: дорога — не
// состояние мира, а то, что животное видит перед собой в этот тик.
struct PathCell {
    int x = 0;
    int y = 0;
};

// Округа хищника: за сколько шагов он доходит до каждой клетки, если идти
// ногами по проходимым клеткам, и куда он не дойдёт вовсе.
//
// Волна расходится от самого животного и не выходит за круг восприятия:
// дальше видимости зверь дороги не знает, и обход длиной в полкарты — это
// уже не "вижу, как дойти", а карта в голове (02_CorePrinciples.md, п.6).
//
// Объект живёт у вызывающей стороны и переиспользуется: за тик волна
// пускается столько раз, сколько в мире голодных хищников, а массивы у неё
// на всю Область. Чтобы не обнулять их перед каждой волной, клетка
// помечается номером волны, а не признаком "посещена".
class Reach {
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
    void roadTo(int targetX, int targetY, std::vector<PathCell>& out) const {
        out.clear();
        if (!reached(targetX, targetY)) {
            return;
        }
        int cx = targetX;
        int cy = targetY;
        while (steps_[cellOf(cx, cy)] > 0) {
            out.push_back(PathCell{cx, cy});
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
    // зверь считает своим участком, а что чужим берегом.
    void reachedCells(std::vector<PathCell>& out) const {
        out.clear();
        for (const auto cell : queue_) {
            out.push_back(PathCell{static_cast<int>(cell % static_cast<std::size_t>(width_)),
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
} // namespace goblins

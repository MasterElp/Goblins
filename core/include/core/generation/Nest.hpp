#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#include "core/Area.hpp"
#include "core/Random.hpp"

namespace goblins {

// Гнездо: как выпустить в мир не рассыпанную по карте сыпь, а кучку,
// живущую в одном месте, — ОДИН закон на всех, кто расселяется.
//
// Живёт здесь, а не внутри TreeSeeding, откуда он взят, по той же причине,
// что и moistureTarget в core/Moisture.hpp: пользуются им уже несколько
// стадий генерации, а две копии одного закона неизбежно разъезжаются
// (CLAUDE.md). Сам закон при этом не знает ни про деревья, ни про животных,
// ни про гоблинов: он принимает две функции — годна ли клетка под центр и
// посадить в клетку — и больше ничего.
//
// Зачем вообще кучка. Рассыпанное поодиночке существо почти всегда
// оказывается единственным на несколько десятков клеток: семя дерева летит
// шесть клеток (core/Trees.hpp), у гоблина округа не шире его зрения, — и
// весь мир начинается как поле одиночек, которым сходиться тысячи тиков,
// если они успеют. Гнездо даёт виду место, где он живёт, с первого тика.
//
// Форма: центр, выбранный придирчиво, и кольца наружу от него. Ровным
// кругом гнездо от этого не становится — его край рвут камни, вода и
// бедная земля, потому что каждую клетку всё равно проверяет тот, кто
// сажает.

// Сколько случайных клеток перебирается в поисках центра. Не множитель от
// числа особей, как в BoulderScatter: ищется одна клетка на гнездо, а не
// место каждому, — зато ищется придирчиво, и на скупой карте перебор может
// уйти впустую весь.
inline constexpr int kNestCenterAttempts = 3000;

// Разложить count штук гнездом радиуса radius. Возвращает, сколько
// разложилось: ноль означает, что виду не нашлось места вовсе — мир для
// него слишком беден.
//
// suitable(x, y) — годна ли клетка под ЦЕНТР. Придирчивость этой проверки и
// делает выбор места осмысленным, поэтому она отдельная от place: центру
// позволено требовать больше, чем краю.
//
// place(x, y) — посадить. false означает "не уместилось", и это не ошибка:
// обход просто идёт дальше. Именно на нём гнездо и заканчивается само,
// когда земля вокруг кончилась.
//
// Ни suitable, ни place не должны трогать random до того, как их позвали:
// порядок розыгрышей здесь — часть закона, и мир на том же seed обязан
// получаться тот же.
template <typename Suitable, typename Place>
int seedNest(const Area& area, int count, int radius, Suitable&& suitable, Place&& place,
             std::uint64_t& random) {
    const int width = area.width();
    const int height = area.height();
    if (width <= 0 || height <= 0 || count <= 0) {
        return 0;
    }

    int centerX = -1;
    int centerY = -1;
    for (int attempt = 0; attempt < kNestCenterAttempts && centerX < 0; ++attempt) {
        const int x = static_cast<int>(randomUnit(random) * static_cast<float>(width)) % width;
        const int y = static_cast<int>(randomUnit(random) * static_cast<float>(height)) % height;
        if (!suitable(x, y)) {
            continue;
        }
        centerX = x;
        centerY = y;
    }
    if (centerX < 0) {
        return 0;
    }

    int placed = 0;
    if (place(centerX, centerY)) {
        ++placed;
    }

    // Кольцо за кольцом наружу. Обход каждого кольца начинается со
    // случайной его клетки: иначе гнездо заполнялось бы всегда с одного
    // угла и на скупой земле получалось бы полумесяцем, глядящим в одну и
    // ту же сторону во всех мирах.
    std::vector<std::pair<int, int>> ring;
    for (int r = 1; r <= radius && placed < count; ++r) {
        ring.clear();
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const int x = centerX + dx;
                const int y = centerY + dy;
                if (area.inBounds(x, y)) {
                    ring.emplace_back(x, y);
                }
            }
        }
        if (ring.empty()) {
            continue;
        }
        const std::size_t start =
            static_cast<std::size_t>(randomBelow(random, static_cast<std::uint64_t>(ring.size())));
        for (std::size_t n = 0; n < ring.size() && placed < count; ++n) {
            const auto& cell = ring[(start + n) % ring.size()];
            if (place(cell.first, cell.second)) {
                ++placed;
            }
        }
    }
    return placed;
}

} // namespace goblins

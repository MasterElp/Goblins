#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <entt/entt.hpp>

namespace goblins {

// Область — квадратная тайловая карта (04_WorldModel.md, п.2).
// Тайл сам по себе пустой контейнер без свойств (04_WorldModel.md, п.3) —
// здесь хранится только список Entity, находящихся на каждом тайле
// (04_WorldModel.md, п.5).
//
// Правило "непроходимый Entity занимает тайл полностью" (04_WorldModel.md,
// п.4) — не свойство тайла, а результат работы System, использующей эту
// структуру для проверки перед размещением. Area лишь хранит признак
// занятости (impassableOccupant), но не решает, кого куда можно ставить.
class Area {
public:
    Area(int width, int height)
        : width_(width), height_(height),
          cells_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {}

    int width() const { return width_; }
    int height() const { return height_; }

    bool inBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    struct Cell {
        std::vector<entt::entity> entities;
        std::optional<entt::entity> impassableOccupant;
    };

    Cell& cellAt(int x, int y) { return cells_[index(x, y)]; }
    const Cell& cellAt(int x, int y) const { return cells_[index(x, y)]; }

    bool isBlocked(int x, int y) const {
        return cellAt(x, y).impassableOccupant.has_value();
    }

    // Регистрирует Entity на тайле (x, y). impassable=true помечает тайл
    // как полностью занятый — до тех пор ни один другой непроходимый
    // Entity туда попасть не может (проверяется вызывающей стороной через
    // isBlocked до вызова place).
    void place(entt::entity entity, int x, int y, bool impassable) {
        auto& cell = cellAt(x, y);
        cell.entities.push_back(entity);
        if (impassable) {
            cell.impassableOccupant = entity;
        }
    }

    // Очищает все тайлы (список Entity и признак непроходимости), не
    // меняя размер Области. Нужен для повторной генерации мира на месте —
    // сами Entity нужно удалить из registry отдельно, до вызова clear();
    // здесь только сброс индекса размещения.
    void clear() {
        cells_.assign(cells_.size(), Cell{});
    }

private:
    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    }

    int width_;
    int height_;
    std::vector<Cell> cells_;
};

} // namespace goblins

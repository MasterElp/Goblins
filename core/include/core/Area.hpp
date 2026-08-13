#pragma once

#include <algorithm>
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

    // Снимает Entity с тайла. Нужен с появлением Entity, которые
    // исчезают во время симуляции (умершее растение, PlantSystem): до
    // них Entity только создавались — при генерации — и жили до конца
    // мира, поэтому обратной операции не требовалось. Без неё в клетке
    // копились бы идентификаторы уничтоженных Entity, а EnTT выдаёт
    // идентификаторы повторно — и проверка "занята ли клетка" в какой-то
    // момент начала бы отвечать про совсем другой объект.
    void remove(entt::entity entity, int x, int y) {
        auto& cell = cellAt(x, y);
        cell.entities.erase(std::remove(cell.entities.begin(), cell.entities.end(), entity), cell.entities.end());
        if (cell.impassableOccupant == entity) {
            cell.impassableOccupant.reset();
        }
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

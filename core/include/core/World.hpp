#pragma once

#include <entt/entt.hpp>

#include "core/Area.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"

namespace goblins {

// Мир на первом этапе — это единственная Область (04_WorldModel.md, п.1),
// поэтому World сразу владеет и ECS-хранилищем (entt::registry), и картой
// (Area). Дополнительная обёртка "Мир из нескольких Областей" вводится на
// будущих этапах и не должна ломать этот класс — она добавится поверх.
//
// На World Entity живут глобальные данные симуляции — не только
// TimeComponent, но и любое "свойство мира" (06_GameLoop.md, п.1a,
// WorldPropertiesComponent), поскольку "Всё является Entity" без
// исключений (02_CorePrinciples.md, п.2) — ни счётчик тиков, ни свойство
// мира не могут быть просто переменной кода. WorldPropertiesComponent
// создаётся здесь же, с значениями по умолчанию, — гарантия, что он
// существует всегда, даже до генерации/загрузки конкретного мира
// (значения выставляет generateTerrain или WorldSave::loadWorld).
class World {
public:
    explicit World(int width = 100, int height = 100)
        : area_(width, height) {
        worldEntity_ = registry_.create();
        registry_.emplace<TimeComponent>(worldEntity_);
        registry_.emplace<WorldPropertiesComponent>(worldEntity_);
    }

    // Полный сброс мира на месте: все Entity удаляются, Область
    // заменяется на новую заданного размера, World Entity создаётся
    // заново с нулевым TimeComponent и свойствами мира по умолчанию.
    // Нужен для загрузки сохранённого мира: размер его Области может
    // отличаться от текущего, а ссылки на сам World (их держат GameLoop и
    // NetworkServer) обязаны остаться валидными — поэтому мир именно
    // сбрасывается, а не пересоздаётся.
    void reset(int width, int height) {
        registry_.clear();
        area_ = Area(width, height);
        worldEntity_ = registry_.create();
        registry_.emplace<TimeComponent>(worldEntity_);
        registry_.emplace<WorldPropertiesComponent>(worldEntity_);
    }

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    entt::entity worldEntity() const { return worldEntity_; }

    Area& area() { return area_; }
    const Area& area() const { return area_; }

private:
    entt::registry registry_;
    entt::entity worldEntity_;
    Area area_;
};

} // namespace goblins

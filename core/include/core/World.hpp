#pragma once

#include <entt/entt.hpp>

#include "core/Area.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/ImpassableComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
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
// (значения выставляет generateTerrain или WorldSave::loadWorld). По той
// же причине здесь создаются и пустые PlantSpeciesComponent /
// AnimalSpeciesComponent (виды травы и виды животных этого мира, заполняют
// seedGrass и seedAnimals): системе не нужно знать, был ли этап заселения
// растительностью и появлялись ли в мире животные — она просто увидит, что
// видов нет.
class World {
public:
    explicit World(int width = 100, int height = 100)
        : area_(width, height) {
        worldEntity_ = registry_.create();
        registry_.emplace<TimeComponent>(worldEntity_);
        registry_.emplace<WorldPropertiesComponent>(worldEntity_);
        registry_.emplace<PlantSpeciesComponent>(worldEntity_);
        registry_.emplace<AnimalSpeciesComponent>(worldEntity_);
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
        registry_.emplace<PlantSpeciesComponent>(worldEntity_);
        registry_.emplace<AnimalSpeciesComponent>(worldEntity_);
    }

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    entt::entity worldEntity() const { return worldEntity_; }

    const Area& area() const { return area_; }

    // --- Размещение Entity в пространстве ---
    //
    // Единственный вход: и PositionComponent, и список Entity на тайле
    // (Area) описывают одно и то же, поэтому изменяться они могут только
    // вместе. Отсюда и то, что Area наружу отдаёт лишь чтение — по
    // отдельности эти две стороны разъезжались бы молча (см. комментарий
    // в Area.hpp).
    //
    // Непроходимость не передаётся параметром, а читается у самого Entity
    // (ImpassableComponent): свойство принадлежит Entity, а не вызову
    // (04_WorldModel.md, п.4), и передаваемый руками флаг мог разойтись с
    // компонентом. Компонент, значит, добавляется ДО размещения.
    //
    // Проверить, свободен ли тайл (area().isBlocked), — задача
    // вызывающего: Area не решает, кого куда можно ставить, и World тоже.

    // Поставить Entity на тайл: выдаёт ему PositionComponent и
    // регистрирует на тайле.
    void place(entt::entity entity, int x, int y) {
        registry_.emplace_or_replace<PositionComponent>(entity, PositionComponent{x, y});
        area_.place(entity, x, y, registry_.all_of<ImpassableComponent>(entity));
    }

    // Перенести Entity на другой тайл. Понадобится всему, что движется;
    // сейчас — гарантия, что снятие со старого тайла нельзя забыть.
    void moveTo(entt::entity entity, int x, int y) {
        if (const auto* position = registry_.try_get<PositionComponent>(entity)) {
            area_.remove(entity, position->x, position->y);
        }
        place(entity, x, y);
    }

    // Убрать Entity из мира: снять с тайла и уничтожить.
    void despawn(entt::entity entity) {
        if (!registry_.valid(entity)) {
            return;
        }
        if (const auto* position = registry_.try_get<PositionComponent>(entity)) {
            area_.remove(entity, position->x, position->y);
        }
        registry_.destroy(entity);
    }

private:
    entt::registry registry_;
    entt::entity worldEntity_;
    Area area_;
};

} // namespace goblins

#pragma once

#include "core/World.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/SoilComponent.hpp"

namespace goblins {

// Положить крупицы минералов перегноем на тайл — один и тот же закон для
// всего, что умирает или испражняется.
//
// Живёт здесь, а не внутри системы, по той же причине, что и moistureTarget
// в core/Moisture.hpp: этим пользуются уже двое (умершее растение в
// PlantSystem, навоз и труп в HerbivoreSystem), а две копии одного закона
// неизбежно разъезжаются. Перегной — не отдельный объект на земле, а
// состояние самой земли, поэтому компонент ложится на терраформирующий
// Entity тайла, рядом с почвой и водой (см. HumusComponent). Если перегной
// там уже есть, крупицы просто складываются: на старом лугу иначе копились
// бы десятки отдельных остатков на одном тайле.
//
// Вызывать только из команды очереди (05_Entity.md, п.5): добавление
// компонента — структурное изменение, и во время обхода систем его быть не
// должно.
inline void depositHumus(World& world, int x, int y, int minerals) {
    if (minerals <= 0 || !world.area().inBounds(x, y)) {
        return;
    }
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!world.registry().all_of<SoilComponent>(tile)) {
            continue;
        }
        if (auto* humus = world.registry().try_get<HumusComponent>(tile)) {
            humus->minerals += minerals;
        } else {
            world.registry().emplace<HumusComponent>(tile, HumusComponent{minerals, 0.0f});
        }
        return;
    }
}

} // namespace goblins

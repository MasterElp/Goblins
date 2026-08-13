#pragma once

#include <vector>

#include "core/components/PlantGenomeComponent.hpp"

namespace goblins {

// Виды травы, существующие в этом мире, — свойство мира (06_GameLoop.md,
// п.1a) из числа "зафиксированных при генерации": набор видов выбирается
// один раз, при создании мира, и System-ами не меняется. Поэтому компонент
// живёт на World Entity, рядом с TimeComponent и
// WorldPropertiesComponent, а не отдельным механизмом "справочник видов".
//
// Архетип вида — это тот же PlantGenomeComponent: у вида нет никаких
// параметров, которых не было бы у отдельного растения. Архетип нужен не
// как "класс объекта" (растение целиком описывается собственным геномом),
// а как центр, вокруг которого мутациям разрешено гулять: потомок
// отклоняется от архетипа не больше чем на полосу kSpeciesBand
// (PlantGenetics), иначе за сотни поколений виды слились бы в один и
// разница стратегий исчезла.
//
// Индекс в archetypes — это PlantGenomeComponent::species.
struct PlantSpeciesComponent {
    std::vector<PlantGenomeComponent> archetypes;
};

} // namespace goblins

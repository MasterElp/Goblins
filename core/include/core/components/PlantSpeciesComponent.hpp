#pragma once

#include <vector>

#include "core/components/PlantGenomeComponent.hpp"

namespace goblins {

// Виды растений, существующие в этом мире, — свойство мира (06_GameLoop.md,
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
// Два списка, а не один, ровно по той же причине, что и у животных
// (AnimalSpeciesComponent): у травы и у деревьев свои таблицы черт и свои
// бюджеты преимуществ (PlantGenetics.hpp), и сравнивать вложения одного с
// вложениями другого незачем — они играют в разные игры.
// PlantGenomeComponent::species — индекс в СВОЁМ списке, в том, который
// соответствует наличию или отсутствию TreeComponent.
struct PlantSpeciesComponent {
    std::vector<PlantGenomeComponent> grasses;
    std::vector<PlantGenomeComponent> trees;
};

} // namespace goblins

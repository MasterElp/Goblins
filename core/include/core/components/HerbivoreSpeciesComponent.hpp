#pragma once

#include <vector>

#include "core/components/HerbivoreGenomeComponent.hpp"

namespace goblins {

// Виды травоядных, существующие в этом мире, — свойство мира
// (06_GameLoop.md, п.1a) из числа "зафиксированных при генерации", ровно как
// PlantSpeciesComponent: набор видов выбирается один раз, при создании мира,
// и System-ами не меняется. Поэтому компонент живёт на World Entity, рядом с
// TimeComponent, WorldPropertiesComponent и видами травы.
//
// Архетип вида — это тот же HerbivoreGenomeComponent: у вида нет параметров,
// которых не было бы у отдельного животного. Архетип нужен как центр, вокруг
// которого мутациям разрешено гулять (kSpeciesBand, core/generation/
// Genetics.hpp), иначе за сотни поколений виды слились бы в один.
//
// Индекс в archetypes — это HerbivoreGenomeComponent::species.
struct HerbivoreSpeciesComponent {
    std::vector<HerbivoreGenomeComponent> archetypes;
};

} // namespace goblins

#pragma once

#include <vector>

#include "core/components/AnimalGenomeComponent.hpp"

namespace goblins {

// Виды животных, существующие в этом мире, — свойство мира
// (06_GameLoop.md, п.1a) из числа "зафиксированных при генерации", ровно как
// PlantSpeciesComponent: набор видов выбирается один раз, при создании мира,
// и System-ами не меняется. Поэтому компонент живёт на World Entity, рядом с
// TimeComponent, WorldPropertiesComponent и видами травы.
//
// Два списка, а не один: у травоядных и хищников свои таблицы черт и свои
// бюджеты преимуществ (core/generation/AnimalGenetics.hpp), а значит и
// сравнивать их между собой незачем — они не конкурируют за одно и то же.
// AnimalGenomeComponent::species — индекс в СВОЁМ списке, в том, который
// соответствует диете животного (HerbivoreComponent / PredatorComponent).
//
// Архетип вида — это тот же AnimalGenomeComponent: у вида нет параметров,
// которых не было бы у отдельного животного. Архетип нужен как центр,
// вокруг которого мутациям разрешено гулять (kSpeciesBand,
// core/generation/Genetics.hpp), иначе за сотни поколений виды слились бы
// в один.
struct AnimalSpeciesComponent {
    std::vector<AnimalGenomeComponent> herbivores;
    std::vector<AnimalGenomeComponent> predators;
};

} // namespace goblins

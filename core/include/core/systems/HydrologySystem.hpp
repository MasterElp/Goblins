#pragma once

#include "core/CommandQueue.hpp"
#include "core/World.hpp"

namespace goblins {

// Постоянно действующий процесс поверх результата TerrainGenerator: вода
// размягчает соседнюю почву, влага вдали от воды со временем высыхает, а сама
// вода медленно течёт по рельефу (HeightComponent) и эродирует его — из-за
// чего очертания рек и прудов постепенно меняются. Идёт в GameLoop сразу
// после TimeSystem (см. server/main.cpp) — вычисляет следующее состояние из
// текущего, ничего не имитирует напрямую (02_CorePrinciples.md, п.15).
void HydrologySystem(World& world, CommandQueue& commands);

} // namespace goblins

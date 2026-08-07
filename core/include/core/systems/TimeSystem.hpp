#pragma once

#include "core/CommandQueue.hpp"
#include "core/World.hpp"

namespace goblins {

// Первая система в порядке выполнения (06_GameLoop.md, п.2): увеличивает
// номер тика на World Entity, чтобы остальные системы этого же тика уже
// видели новое значение.
void TimeSystem(World& world, CommandQueue& commands);

} // namespace goblins

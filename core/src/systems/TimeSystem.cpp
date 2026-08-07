#include "core/systems/TimeSystem.hpp"

#include "core/components/TimeComponent.hpp"

namespace goblins {

void TimeSystem(World& world, CommandQueue&) {
    auto& time = world.registry().get<TimeComponent>(world.worldEntity());
    time.tick += 1;
}

} // namespace goblins

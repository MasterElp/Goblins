#pragma once

#include <functional>
#include <vector>

namespace goblins {

class World;

// Создание/удаление Entity и добавление/удаление компонентов не происходит
// мгновенно во время выполнения систем — команда ставится в очередь и
// применяется после того, как все системы тика отработали (05_Entity.md,
// п.5). Это предотвращает изменение коллекций во время их обхода системами.
class CommandQueue {
public:
    using Command = std::function<void(World&)>;

    void enqueue(Command command) {
        commands_.push_back(std::move(command));
    }

    void resolve(World& world) {
        for (auto& command : commands_) {
            command(world);
        }
        commands_.clear();
    }

    bool empty() const { return commands_.empty(); }

private:
    std::vector<Command> commands_;
};

} // namespace goblins

#pragma once

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "core/CommandQueue.hpp"
#include "core/System.hpp"
#include "core/World.hpp"

namespace goblins {

// Один тик = выполнение всех систем в фиксированном порядке, затем
// разрешение очереди команд (06_GameLoop.md, п.2–3). Цикл пошаговый:
// следующий тик начинается сразу после завершения предыдущего; если тик
// выполнился быстрее целевого интервала, перед следующим добавляется
// задержка (06_GameLoop.md, п.4).
class GameLoop {
public:
    explicit GameLoop(World& world, std::chrono::milliseconds targetInterval = std::chrono::milliseconds(0))
        : world_(world), targetInterval_(targetInterval) {}

    // Системы добавляются в том порядке, в котором должны выполняться —
    // порядок фиксирован и не меняется динамически (06_GameLoop.md, п.3).
    void addSystem(System system) {
        systems_.push_back(std::move(system));
    }

    void tick() {
        for (auto& system : systems_) {
            system(world_, commandQueue_);
        }
        commandQueue_.resolve(world_);

        if (onTickComplete) {
            onTickComplete(world_);
        }
    }

    // shouldContinue проверяется перед каждым тиком; run() не знает,
    // сколько тиков нужно выполнить — это решает вызывающая сторона.
    void run(const std::function<bool()>& shouldContinue) {
        while (shouldContinue()) {
            const auto start = std::chrono::steady_clock::now();
            tick();
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < targetInterval_) {
                std::this_thread::sleep_for(targetInterval_ - elapsed);
            }
        }
    }

    // Хук для внешнего наблюдателя (например, сервера, печатающего лог).
    // Сам GameLoop не знает, что с этим состоянием делают дальше — это
    // соответствует принципу "Все взаимодействия происходят через
    // интерфейсы" (02_CorePrinciples.md).
    std::function<void(const World&)> onTickComplete;

private:
    World& world_;
    CommandQueue commandQueue_;
    std::vector<System> systems_;
    std::chrono::milliseconds targetInterval_;
};

} // namespace goblins

#pragma once

#include <cstdint>

namespace goblins {

// Согласно 06_GameLoop.md, п.1: счётчик тиков хранится как данные на
// World Entity, а не как переменная кода игрового цикла.
struct TimeComponent {
    std::uint64_t tick = 0;
};

} // namespace goblins

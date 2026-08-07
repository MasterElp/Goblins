#pragma once

namespace goblins {

// Координаты Entity в Области (04_WorldModel.md, п.2: две оси, без высоты).
struct PositionComponent {
    int x = 0;
    int y = 0;
};

} // namespace goblins

#pragma once

#include <functional>

namespace goblins {

class World;
class CommandQueue;

// Система — функция, вычисляющая следующее состояние компонентов на основе
// текущего (05_Entity.md, п.3). Система не хранит собственное состояние
// между тиками и не вызывает другие системы напрямую (05_Entity.md, п.6).
using System = std::function<void(World&, CommandQueue&)>;

} // namespace goblins

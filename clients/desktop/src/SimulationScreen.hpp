#pragma once

#include "AppScreen.hpp"
#include "NetworkClient.hpp"
#include "config/Config.hpp"

namespace SimulationScreen {

// Прокручиваемый просмотр мира (WASD/стрелки), пауза по P. Размер тайла
// на экране — config.tile_size, умноженный на масштаб (колесо мыши,
// зум к точке под курсором); при изменении размера окна меняется
// только количество видимых тайлов, не сам масштаб.
AppScreen draw(NetworkClient& network, const goblins::ClientConfig& config);

} // namespace SimulationScreen

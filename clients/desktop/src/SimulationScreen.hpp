#pragma once

#include "AppScreen.hpp"
#include "NetworkClient.hpp"
#include "config/Config.hpp"

namespace SimulationScreen {

// Прокручиваемый просмотр мира (WASD/стрелки), пауза по P. В отличие от
// экрана генерации, размер тайла здесь фиксирован (config.tile_size) —
// при изменении размера окна меняется не масштаб, а сколько тайлов
// видно одновременно.
AppScreen draw(NetworkClient& network, const goblins::ClientConfig& config);

} // namespace SimulationScreen

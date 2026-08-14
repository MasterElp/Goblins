#pragma once

#include <string>

#include "AppScreen.hpp"
#include "NetworkClient.hpp"
#include "config/Config.hpp"

namespace SimulationScreen {

// Прокручиваемый просмотр мира (WASD/стрелки), пауза по P. Размер тайла
// на экране — config.tile_size, умноженный на масштаб (колесо мыши,
// зум к точке под курсором); при изменении размера окна меняется
// только количество видимых тайлов, не сам масштаб.
//
// config не const: масштаб и включённые слои почвы сохраняются в него и
// на диск (configPath) сразу при изменении — как графические настройки
// на экране "Настройки" (SettingsScreen), без отдельной кнопки "Сохранить".
AppScreen draw(NetworkClient& network, goblins::ClientConfig& config, const std::string& configPath);

} // namespace SimulationScreen

#pragma once

#include <string>

#include "AppScreen.hpp"
#include "config/Config.hpp"

namespace SettingsScreen {

// Разрешение и оконный/полноэкранный режим. Изменения применяются сразу
// (SetWindowSize/ToggleFullscreen) и сохраняются в config.json — не
// нужно отдельной кнопки "Сохранить".
AppScreen draw(goblins::ClientConfig& config, const std::string& configPath);

} // namespace SettingsScreen

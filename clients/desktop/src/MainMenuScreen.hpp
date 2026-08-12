#pragma once

#include "AppScreen.hpp"

namespace MainMenuScreen {

// Рисует меню на весь текущий размер окна. Возвращает экран, на который
// нужно перейти, если пользователь нажал один из пунктов — иначе тот же
// AppScreen::MainMenu (остаться на месте).
AppScreen draw();

} // namespace MainMenuScreen

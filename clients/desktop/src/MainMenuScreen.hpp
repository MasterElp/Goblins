#pragma once

#include "AppScreen.hpp"
#include "NetworkClient.hpp"

namespace MainMenuScreen {

// Рисует меню на весь текущий размер окна. Возвращает экран, на который
// нужно перейти, если пользователь нажал один из пунктов — иначе тот же
// AppScreen::MainMenu (остаться на месте). Кнопка "Simulation" ведёт на
// экран выбора сохранённого мира и заранее запрашивает у сервера их
// список (см. NetworkClient::sendListWorlds).
AppScreen draw(NetworkClient& network);

} // namespace MainMenuScreen

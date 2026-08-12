#pragma once

#include "AppScreen.hpp"
#include "NetworkClient.hpp"

namespace WorldSelectScreen {

// Выбор мира перед симуляцией: список сохранённых на сервере миров
// (сообщение world_list) плюс кнопка "New world".
//
// Если сохранённых миров нет вообще, экран не заставляет пользователя
// нажимать "New world" — он сам просит сервер сгенерировать новый мир и
// сразу уходит в симуляцию. Именно поэтому важно различать "миров нет" и
// "список ещё не пришёл" (WorldState::worldsReceived): по второму
// генерировать нельзя, иначе каждый запуск клиента плодил бы лишний мир.
AppScreen draw(NetworkClient& network);

} // namespace WorldSelectScreen

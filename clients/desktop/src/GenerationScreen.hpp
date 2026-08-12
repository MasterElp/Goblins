#pragma once

#include "AppScreen.hpp"
#include "NetworkClient.hpp"
#include "SettingsPanel.hpp"

namespace GenerationScreen {

// Этап 1 генерации — почва и вода (следующие этапы, растительность и
// т.д., добавятся позже). Вся карта всегда помещается в отведённую под
// неё область целиком: размер тайла каждый кадр пересчитывается под
// текущий размер окна, а не берётся из конфигурации — поэтому при
// изменении размера окна меняется только отображение, без прокрутки.
AppScreen draw(NetworkClient& network, SettingsPanel& panel);

} // namespace GenerationScreen

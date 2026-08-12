#pragma once

#include <raylib.h>

#include "config/Config.hpp"

// Панель настроек генерации — immediate-mode GUI (raygui) поверх карты.
// Инкапсулирует редактируемую копию параметров и прокрутку; сама сеть не
// трогает — только возвращает готовый RegenerationRequest, когда
// пользователь нажимает "Regenerate" (main.cpp сам решает, что с этим
// запросом делать — отправить на сервер).
class SettingsPanel {
public:
    // Заполняет редактируемые поля текущими значениями с сервера. Не
    // перезаписывает уже начатое пользователем редактирование, если
    // force=false (иначе очередной снапшот с сервера сбрасывал бы
    // недописанные правки на каждый тик).
    void loadFrom(const goblins::RegenerationRequest& current, bool force = false);

    // Рисует панель, если visible. Возвращает true и заполняет
    // outRequest, если пользователь нажал "Regenerate".
    bool draw(Rectangle bounds, goblins::RegenerationRequest& outRequest);

    bool visible = false;

private:
    goblins::RegenerationRequest edited_{};
    Vector2 scroll_{0, 0};
    bool loaded_ = false;
};

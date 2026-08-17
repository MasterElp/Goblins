#pragma once

#include <raylib.h>

#include "config/Config.hpp"

// Панель настроек генерации — immediate-mode GUI (raygui). Используется
// как сворачиваемая панель параметров на экране мира (WorldScreen,
// клавиша G). Инкапсулирует
// редактируемую копию параметров и прокрутку; сама сеть не трогает —
// только возвращает готовый RegenerationRequest, когда пользователь
// нажимает "Regenerate" (вызывающая сторона сама решает, что с этим
// запросом делать — отправить на сервер).
class SettingsPanel {
public:
    // Заполняет редактируемые поля текущими значениями с сервера. Не
    // перезаписывает уже начатое пользователем редактирование, если
    // force=false (иначе очередной снапшот с сервера сбрасывал бы
    // недописанные правки на каждый тик).
    void loadFrom(const goblins::RegenerationRequest& current, bool force = false);

    // Рисует панель. outRequest заполняется всегда — это текущее
    // содержимое ползунков; возвращает true, если пользователь нажал
    // "Regenerate". Выставляет outSaveRequested в true, если пользователь
    // нажал "Save values" (вызывающая сторона сама решает, что с этим
    // делать — отправить save_generation_config с теми же значениями:
    // "Save values" сохраняет набранное на панели, а не то, чем
    // сгенерирован текущий мир).
    // worldGenerated меняет только подпись главной кнопки: в пустом мире
    // (сервер только поднялся, игрок нажал "New world") она называется
    // "Create world", а не "Regenerate" — перегенерировать там нечего.
    bool draw(Rectangle bounds, goblins::RegenerationRequest& outRequest, bool& outSaveRequested,
              bool worldGenerated);

private:
    goblins::RegenerationRequest edited_{};
    Vector2 scroll_{0, 0};
    bool loaded_ = false;
    // Размер Области набирается руками, а не выбран кнопкой-пресетом.
    // Живёт здесь, а не в самом запросе: это состояние панели ("покажи
    // ползунки"), а не свойство мира — мир 200x200, набранный вручную, и
    // мир 200x200, выбранный кнопкой, одинаковы.
    bool customArea_ = false;
};

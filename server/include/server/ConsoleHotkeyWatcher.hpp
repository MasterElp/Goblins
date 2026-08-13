#pragma once

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace goblins {

// Неблокирующее чтение с консоли для одной горячей клавиши — Ctrl+L,
// байт 0x0C в "сыром" вводе что на Windows (conio.h), что на POSIX
// (терминал в raw-режиме), поэтому проверка одна и та же на обеих
// платформах. Сервер — консольное приложение без своего оконного цикла
// событий (в отличие от клиента на raylib), поэтому опрашивается вручную,
// раз за итерацию GameLoop (см. server/main.cpp).
class ConsoleHotkeyWatcher {
public:
    ConsoleHotkeyWatcher() {
#if !defined(_WIN32)
        if (tcgetattr(STDIN_FILENO, &originalTermios_) == 0) {
            termios raw = originalTermios_;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                rawModeSet_ = true;
            }
        }
#endif
    }

    ~ConsoleHotkeyWatcher() {
#if !defined(_WIN32)
        if (rawModeSet_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios_);
        }
#endif
    }

    ConsoleHotkeyWatcher(const ConsoleHotkeyWatcher&) = delete;
    ConsoleHotkeyWatcher& operator=(const ConsoleHotkeyWatcher&) = delete;

    // true, если с прошлого вызова был нажат Ctrl+L. Не блокирует —
    // вычитывает и отбрасывает весь доступный сейчас ввод, поэтому
    // безопасно звать каждую итерацию цикла.
    bool ctrlLPressed() {
        bool pressed = false;
#if defined(_WIN32)
        while (_kbhit()) {
            if (_getch() == 0x0C) {
                pressed = true;
            }
        }
#else
        if (!rawModeSet_) {
            return false;
        }
        char c = 0;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 0x0C) {
                pressed = true;
            }
        }
#endif
        return pressed;
    }

private:
#if !defined(_WIN32)
    termios originalTermios_{};
    bool rawModeSet_ = false;
#endif
};

} // namespace goblins

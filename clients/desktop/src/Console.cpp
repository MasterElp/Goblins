#include "Console.hpp"

#include <cstdio>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace goblins {

#if defined(_WIN32)

void consoleInit() {
    // На Windows 10+ обычная консоль поддерживает ANSI-escape только
    // после явного включения этого режима.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

void consoleRestore() {
    // Ничего не меняли в режиме ввода — восстанавливать нечего.
}

bool consoleKeyPressed() {
    return _kbhit() != 0;
}

char consoleReadKey() {
    return static_cast<char>(_getch());
}

#else

namespace {
termios g_originalTermios{};
}

void consoleInit() {
    tcgetattr(STDIN_FILENO, &g_originalTermios);
    termios raw = g_originalTermios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void consoleRestore() {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_originalTermios);
}

bool consoleKeyPressed() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval timeout{0, 0};
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &timeout) > 0;
}

char consoleReadKey() {
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return 0;
    }
    return c;
}

#endif

void consoleClear() {
    std::fputs("\033[2J\033[H", stdout);
}

} // namespace goblins

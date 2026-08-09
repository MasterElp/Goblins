#pragma once

namespace goblins {

// Клиент — чистый C++: никаких GUI-библиотек, только терминал.
// Здесь минимально необходимое: неблокирующее чтение одной клавиши и
// очистка экрана. Реализация платформо-зависима (conio.h на Windows,
// termios на POSIX), но интерфейс — нет.

// Переводит терминал в нужный режим (raw-режим без эха на POSIX).
// Обязательно вызвать один раз перед consoleKeyPressed()/consoleReadKey().
void consoleInit();

// Возвращает терминал в исходное состояние. Вызвать перед выходом.
void consoleRestore();

// true, если есть непрочитанная клавиша (не блокирует).
bool consoleKeyPressed();

// Читает одну клавишу. Вызывать только после consoleKeyPressed() == true.
char consoleReadKey();

// Очищает экран и переносит курсор в левый верхний угол (ANSI-escape).
void consoleClear();

} // namespace goblins

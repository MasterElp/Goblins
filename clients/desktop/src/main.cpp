#include <chrono>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <raylib.h>

#include "AppScreen.hpp"
#include "MainMenuScreen.hpp"
#include "NetworkClient.hpp"
#include "SettingsPanel.hpp"
#include "SettingsScreen.hpp"
#include "WorldScreen.hpp"
#include "WorldSelectScreen.hpp"
#include "config/Config.hpp"
#include "Version.hpp"

// Клиент — графическое приложение (raylib), не консоль. Подключается
// только по WebSocket-протоколу сервера (07_TechStack.md, п.6) — никакого
// доступа к core. После запуска показывает меню (Settings / World
// Generation / Simulation) — сама отрисовка и логика каждого экрана
// вынесены в отдельные модули, main.cpp только переключает между ними.

int main(int argc, char** argv) {
    const std::string configPath = argc > 1 ? argv[1] : goblins::defaultConfigPathNextToExecutable();
    goblins::ensureConfigExists<goblins::ClientConfig>(configPath);
    auto config = goblins::loadClientConfig(configPath);

    ix::initNetSystem();

    // Одно соединение на всё приложение — устанавливается сразу, до
    // показа меню, чтобы к моменту перехода в "Генерацию" или
    // "Симуляцию" данные уже были на подходе.
    NetworkClient network;
    network.connect(config.host, config.port);

    // Версия в заголовке окна — единственный надёжный способ на глаз
    // отличить только что собранный .exe от того, что уже был запущен:
    // окно можно не пересоздавать между запусками (тот же .exe путь),
    // содержимое биндится намертво при InitWindow.
    const std::string windowTitle = std::string("Goblins - World Simulator v") + goblins::kAppVersion;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(config.window_width, config.window_height, windowTitle.c_str());
    // Esc — по умолчанию "exit key" в raylib (сам взводит WindowShouldClose,
    // в обход экранов). Экраны сами решают, что делать по Esc (вернуться в
    // меню, снять паузу и т.п.), поэтому дефолтное поведение отключаем.
    SetExitKey(KEY_NULL);
    // Тридцать кадров, а не шестьдесят. Мир меняется не чаще, чем раз в
    // snapshot_interval_ms (сотня миллисекунд, то есть десять раз в
    // секунду), и всё, что рисуется чаще, — это одна и та же картина по
    // нескольку раз. Совсем опуститься до частоты мира нельзя: кадр
    // обслуживает ещё и руку — перетаскивание карты, наведение,
    // нажатия, — а десять кадров в секунду рука чувствует как задержку.
    // Тридцать — три кадра на одно состояние мира и 33 мс отклика, чего
    // рука уже не замечает; вдвое меньше работы видеокарте и вдвое
    // меньше нагрева ноутбука на пустом месте.
    SetTargetFPS(30);
    if (config.fullscreen) {
        ToggleFullscreen();
    }

    SettingsPanel generationPanel;
    AppScreen screen = AppScreen::MainMenu;

    // Крестик окна не закрывает приложение сам: WindowShouldClose() лишь
    // сообщает о нажатии (raylib сбрасывает признак на следующем кадре),
    // а решает экран. На экране мира это вопрос "сохранить мир перед
    // выходом?" — несохранённый мир иначе терялся бы молча; на остальных
    // экранах сохранять нечего, и приложение закрывается сразу.
    bool closeRequested = false;
    // Хочет ли пользователь видеть мир прямо сейчас. Не то же, что пауза:
    // мир продолжает жить, просто его состояние сюда не едет. Причин три,
    // и все три сходятся в одно решение — свёрнутое окно (смотреть некуда),
    // окно без фокуса (смотрят в другое) и клавиша (смотреть не хочу).
    // Место здесь, а не на экране мира: свернуть окно можно с любого
    // экрана, а состояние мира сервер шлёт независимо от того, какой экран
    // сейчас открыт.
    //
    // Дороже всего тут не трафик, а сборка снимка на сервере: пока не
    // смотрит НИКТО, сервер её не делает вовсе.
    bool watchByChoice = true;
    while (screen != AppScreen::Exit) {
        if (WindowShouldClose()) {
            closeRequested = true;
        }
        if (IsKeyPressed(KEY_H)) {
            watchByChoice = !watchByChoice;
        }
        network.sendUpdatesEnabled(watchByChoice && !IsWindowMinimized() && IsWindowFocused());

        BeginDrawing();

        switch (screen) {
            case AppScreen::MainMenu:
                screen = MainMenuScreen::draw(network);
                break;
            case AppScreen::Settings:
                screen = SettingsScreen::draw(config, configPath);
                break;
            case AppScreen::WorldSelect:
                screen = WorldSelectScreen::draw(network);
                break;
            case AppScreen::World:
                // closeRequested экран мира забирает себе: он же и задаёт
                // вопрос о сохранении.
                screen = WorldScreen::draw(network, config, configPath, generationPanel, closeRequested);
                break;
            case AppScreen::Exit:
                break;
        }

        EndDrawing();

        if (closeRequested && screen != AppScreen::World) {
            break;
        }
    }

    CloseWindow();

    // Закрытие окна (крестик/Alt+F4) не проходит через кнопку "Back" ни на
    // экране симуляции, ни на экране генерации (там теперь тоже можно
    // запустить мир кнопкой Start) — без этого сервер продолжил бы тикать
    // мир, за которым уже никто не наблюдает. Безусловно и безопасно:
    // stop_simulation лишь гарантирует паузу, что бы ни показывал текущий
    // экран. sendStopSimulation() лишь ставит сообщение в очередь отправки
    // IXWebSocket (не блокирует) — короткая пауза перед disconnect() даёт
    // фоновому потоку сокета шанс реально отправить его до разрыва
    // соединения.
    network.sendStopSimulation();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    network.disconnect();
    ix::uninitNetSystem();

    return 0;
}

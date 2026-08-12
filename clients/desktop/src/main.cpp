#include <string>

#include <ixwebsocket/IXNetSystem.h>
#include <raylib.h>

#include "AppScreen.hpp"
#include "GenerationScreen.hpp"
#include "MainMenuScreen.hpp"
#include "NetworkClient.hpp"
#include "SettingsPanel.hpp"
#include "SettingsScreen.hpp"
#include "SimulationScreen.hpp"
#include "config/Config.hpp"

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

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(config.window_width, config.window_height, "Goblins - World Simulator");
    // Esc — по умолчанию "exit key" в raylib (сам взводит WindowShouldClose,
    // в обход экранов). Экраны сами решают, что делать по Esc (вернуться в
    // меню, снять паузу и т.п.), поэтому дефолтное поведение отключаем.
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    if (config.fullscreen) {
        ToggleFullscreen();
    }

    SettingsPanel generationPanel;
    AppScreen screen = AppScreen::MainMenu;

    while (!WindowShouldClose()) {
        BeginDrawing();

        switch (screen) {
            case AppScreen::MainMenu:
                screen = MainMenuScreen::draw(network);
                break;
            case AppScreen::Settings:
                screen = SettingsScreen::draw(config, configPath);
                break;
            case AppScreen::WorldGeneration:
                screen = GenerationScreen::draw(network, generationPanel);
                break;
            case AppScreen::Simulation:
                screen = SimulationScreen::draw(network, config);
                break;
        }

        EndDrawing();
    }

    CloseWindow();

    network.disconnect();
    ix::uninitNetSystem();

    return 0;
}

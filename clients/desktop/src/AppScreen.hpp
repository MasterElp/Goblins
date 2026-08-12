#pragma once

// Экраны приложения. Меню — точка входа после запуска; остальные три
// пункта меню ведут в соответствующий экран. Назад — по Esc, из любого
// экрана в меню.
enum class AppScreen {
    MainMenu,
    Settings,
    WorldGeneration,
    Simulation,
};

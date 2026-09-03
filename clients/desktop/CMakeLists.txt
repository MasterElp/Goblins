cmake_minimum_required(VERSION 3.21)
project(GoblinsDesktopClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Клиент — отдельный CMake-проект (собирается независимо от core/server,
# 07_TechStack.md, п.6), но зависимости у него общие с сервером. Чтобы код
# и описание сборки vendored-библиотек не повторялись в двух местах,
# подключаем общие third_party/ и shared/ из корня репозитория.
# GOBLINS_WITH_RAYLIB — только клиенту нужна графика, сервер её не тянет.
set(GOBLINS_WITH_RAYLIB ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party ${CMAKE_CURRENT_BINARY_DIR}/third_party)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../shared ${CMAKE_CURRENT_BINARY_DIR}/shared)

add_executable(client
    src/main.cpp
    src/NetworkClient.cpp
    src/MapTexture.cpp
    src/Assets.cpp
    src/SpriteAtlas.cpp
    src/TreeSprites.cpp
    src/BuildSprites.cpp
    src/GoblinSprites.cpp
    src/PlantSprites.cpp
    src/WolfSprites.cpp
    src/DeerSprites.cpp
    src/ConstantsOverlay.cpp
    src/PopulationGraph.cpp
    src/GenomeGraph.cpp
    src/InfoPanel.cpp
    src/KeysPanel.cpp
    src/SettingsPanel.cpp
    src/MainMenuScreen.cpp
    src/SettingsScreen.cpp
    src/WorldSelectScreen.cpp
    src/WorldScreen.cpp
)

target_link_libraries(client PRIVATE nlohmann_json ixwebsocket shared_config raylib raygui)

# Ресурсы кладутся рядом с исполняемым файлом: клиент ищет их именно там
# (Assets::directory), потому что рабочая директория — не то же самое, что
# расположение exe, и запущенный ярлыком клиент иначе не нашёл бы ничего.
# Копирование после каждой сборки, а не при установке: правку рисунка надо
# видеть следующим же запуском, не собирая пакет.
add_custom_command(TARGET client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:client>/assets"
    COMMENT "Copying assets next to the client")

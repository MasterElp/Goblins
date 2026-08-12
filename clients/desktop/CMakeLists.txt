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
    src/SettingsPanel.cpp
)

target_link_libraries(client PRIVATE nlohmann_json ixwebsocket shared_config raylib raygui)

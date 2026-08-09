cmake_minimum_required(VERSION 3.21)
project(Goblins LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/CPM.cmake)

CPMAddPackage(
    NAME EnTT
    GITHUB_REPOSITORY skypjack/entt
    VERSION 3.13.2
    OPTIONS
        "ENTT_INSTALL OFF"
        "ENTT_BUILD_TESTING OFF"
        "ENTT_BUILD_DOCS OFF"
        "ENTT_BUILD_EXAMPLE OFF"
)

# Сетевой слой и сериализация — только для server (07_TechStack.md, п.4).
# core их не видит и не должен видеть.
CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    VERSION 3.11.3
    OPTIONS "JSON_BuildTests OFF"
)

CPMAddPackage(
    NAME IXWebSocket
    GITHUB_REPOSITORY machinezone/IXWebSocket
    VERSION 11.4.5
    OPTIONS
        "USE_TLS OFF"
        "USE_ZLIB OFF"
        "BUILD_DEMO OFF"
        "IXWEBSOCKET_INSTALL OFF"
)

add_subdirectory(core)
add_subdirectory(server)

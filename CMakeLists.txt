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

add_subdirectory(core)
add_subdirectory(server)

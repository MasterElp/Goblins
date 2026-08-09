cmake_minimum_required(VERSION 3.21)
project(Goblins LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Зависимости — vendored в third_party/, сборка не обращается к сети.
add_subdirectory(third_party)

add_subdirectory(core)
add_subdirectory(server)

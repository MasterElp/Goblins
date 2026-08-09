# Небольшой общий код, который используют и server, и clients/desktop, но
# который не является внешней зависимостью (в отличие от third_party/).
# Тот же приём, что и с third_party: описание — один раз здесь, оба
# независимых CMake-проекта подключают через add_subdirectory.
add_library(shared_config INTERFACE)
target_include_directories(shared_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shared_config INTERFACE nlohmann_json)

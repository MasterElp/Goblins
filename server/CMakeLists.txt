add_executable(goblins_server src/main.cpp)

target_link_libraries(goblins_server PRIVATE core)
target_compile_features(goblins_server PRIVATE cxx_std_20)

add_executable(goblins_server
    src/main.cpp
    src/NetworkServer.cpp
)

target_include_directories(goblins_server PRIVATE include)
target_link_libraries(goblins_server PRIVATE core nlohmann_json::nlohmann_json ixwebsocket)
target_compile_features(goblins_server PRIVATE cxx_std_20)

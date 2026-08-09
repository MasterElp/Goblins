add_executable(server
    src/main.cpp
    src/NetworkServer.cpp
)

target_include_directories(server PRIVATE include)
target_link_libraries(server PRIVATE core nlohmann_json ixwebsocket)
target_compile_features(server PRIVATE cxx_std_20)

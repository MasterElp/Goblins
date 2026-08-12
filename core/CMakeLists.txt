add_library(core STATIC
    src/systems/TimeSystem.cpp
    src/generation/BoulderScatter.cpp
    src/generation/TerrainGenerator.cpp
)

target_include_directories(core PUBLIC include)
target_link_libraries(core PUBLIC EnTT PRIVATE FastNoiseLite)
target_compile_features(core PUBLIC cxx_std_20)

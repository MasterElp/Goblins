add_library(core STATIC
    src/Diagnostics.cpp
    src/Simulation.cpp
    src/systems/TimeSystem.cpp
    src/systems/HydrologySystem.cpp
    src/systems/PlantSystem.cpp
    src/systems/HerbivoreSystem.cpp
    src/generation/BoulderScatter.cpp
    src/generation/TerrainGenerator.cpp
    src/generation/PlantGenetics.cpp
    src/generation/GrassSeeding.cpp
    src/generation/HerbivoreGenetics.cpp
    src/generation/HerbivoreSeeding.cpp
)

target_include_directories(core PUBLIC include)
target_link_libraries(core PUBLIC EnTT PRIVATE FastNoiseLite)
target_compile_features(core PUBLIC cxx_std_20)

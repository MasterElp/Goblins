#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "platform/ExecutablePath.hpp"

namespace goblins {

// Конфигурации сервера и клиента разделены и живут каждая рядом со своим
// бинарником — не в одном общем файле. Общее между ними — только сам
// механизм чтения/автогенерации (ниже), не набор полей: серверу не нужно
// знать про размер окна просмотра клиента, а клиенту — про seed генерации
// булыжников.

struct AreaSize {
    int width = 100;
    int height = 100;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AreaSize, width, height)

struct ViewSize {
    int width = 60;
    int height = 25;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ViewSize, width, height)

// Зеркало core::TerrainParams (core/generation/TerrainParams.hpp) — но
// JSON-сериализуемое. Дублирование полей осознанное: core не знает о
// JSON и конфигурации вообще (07_TechStack.md, п.6), поэтому именно
// server (main.cpp) переносит значения из этой структуры в
// core::TerrainParams перед вызовом generateTerrain. Имена и значения по
// умолчанию должны совпадать с core::TerrainParams.
struct TerrainConfig {
    float height_noise_frequency = 0.02f;
    float rock_noise_frequency = 0.05f;
    float compaction_noise_frequency = 0.04f;
    float moisture_noise_frequency = 0.03f;

    int noise_octaves = 4;
    float noise_lacunarity = 2.0f;
    float noise_gain = 0.5f;

    float rock_height_bump = 0.35f;
    float compaction_height_bump = 0.25f;

    float river_threshold = 55.0f;
    float river_depth_base = 0.3f;
    float river_depth_range = 2.2f;
    float edge_inflow_max = 45.0f;

    float min_pond_depth = 0.01f;
    int min_pond_size = 1;
    int max_pond_size = 0;
    float pond_depth_scale = 4.0f;

    float moisture_falloff = 8.0f;
    float water_moisture_boost = 0.7f;
    float rock_moisture_reduction = 0.3f;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TerrainConfig, height_noise_frequency, rock_noise_frequency,
                                    compaction_noise_frequency, moisture_noise_frequency, noise_octaves,
                                    noise_lacunarity, noise_gain, rock_height_bump, compaction_height_bump,
                                    river_threshold, river_depth_base, river_depth_range, edge_inflow_max,
                                    min_pond_depth, min_pond_size, max_pond_size, pond_depth_scale,
                                    moisture_falloff, water_moisture_boost, rock_moisture_reduction)

struct ServerConfig {
    std::string host = "127.0.0.1";
    int port = 9002;

    AreaSize area{};

    unsigned terrain_seed = 54321;
    TerrainConfig terrain{};

    int boulder_count = 40;
    unsigned boulder_seed = 12345;

    int tick_interval_ms = 200;
    // Отрицательное значение — тикать бесконечно (мир существует
    // независимо от наблюдателя, 02_CorePrinciples.md, п.1).
    int tick_count = -1;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ServerConfig, host, port, area, terrain_seed, terrain, boulder_count,
                                    boulder_seed, tick_interval_ms, tick_count)

// Подмножество ServerConfig, которое можно перегенерировать вживую по
// сети (протокол "regenerate", см. NetworkServer.hpp) без пересоздания
// World: размер Области — нет, она фиксирована при создании World и
// затрагивает GameLoop/NetworkServer, которые держат на неё ссылку;
// террейн и булыжники — да, это просто новый набор Entity на том же
// Area.
struct RegenerationRequest {
    unsigned terrain_seed = 54321;
    TerrainConfig terrain{};
    int boulder_count = 40;
    unsigned boulder_seed = 12345;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RegenerationRequest, terrain_seed, terrain, boulder_count, boulder_seed)

struct ClientConfig {
    std::string host = "127.0.0.1";
    int port = 9002;

    // Сколько тайлов видно в окне просмотра и с какой скоростью (тайлов в
    // секунду) прокручивать при зажатой клавише.
    ViewSize view{};
    int scroll_step = 5;

    // Размер тайла в пикселях на экране.
    int tile_size = 16;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ClientConfig, host, port, view, scroll_step, tile_size)

namespace detail {

template <typename Config>
Config loadConfigFile(const std::string& path) {
    Config config;

    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    try {
        nlohmann::json json;
        file >> json;
        config = json.get<Config>();
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Config file '" << path << "' is not valid (" << e.what() << "), using defaults.\n";
    }

    return config;
}

template <typename Config>
void writeConfigFile(const std::string& path, const Config& config) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Could not write default config to '" << path << "'.\n";
        return;
    }
    nlohmann::json json = config;
    file << json.dump(4) << "\n";
}

} // namespace detail

// Если файла по данному пути нет — создаёт его со значениями по умолчанию.
// Конфигурация должна быть удобной (самодокументируемой), а не
// обязательной: отсутствие файла — не ошибка ни на чтении, ни здесь.
template <typename Config>
void ensureConfigExists(const std::string& path) {
    if (std::filesystem::exists(path)) {
        return;
    }
    std::cout << "Config '" << path << "' not found, creating with defaults.\n";
    detail::writeConfigFile(path, Config{});
}

inline ServerConfig loadServerConfig(const std::string& path) {
    return detail::loadConfigFile<ServerConfig>(path);
}

inline ClientConfig loadClientConfig(const std::string& path) {
    return detail::loadConfigFile<ClientConfig>(path);
}

// Путь к config.json рядом с текущим исполняемым файлом — конфигурация
// генерируется и живёт там же, где сам бинарник, а не в рабочей
// директории, из которой его запустили.
inline std::string defaultConfigPathNextToExecutable() {
    return (getExecutableDirectory() / "config.json").string();
}

} // namespace goblins

#pragma once

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace goblins {

// Сервер и клиент читают один и тот же config.json, чтобы адрес и порт не
// расходились (07_TechStack.md — все взаимодействия через интерфейсы;
// конфигурация — часть этого интерфейса, а не догадка каждой стороны).
// Если файла нет или в нём не хватает поля — используется значение по
// умолчанию, а не ошибка: конфигурация должна быть удобной, а не
// обязательной.
struct AppConfig {
    std::string host = "127.0.0.1";
    int port = 9002;

    int areaWidth = 100;
    int areaHeight = 100;

    int boulderCount = 40;
    unsigned boulderSeed = 12345;

    int tickIntervalMs = 200;
    // Отрицательное значение — тикать бесконечно (мир существует
    // независимо от наблюдателя, 02_CorePrinciples.md, п.1).
    int tickCount = -1;

    int viewWidth = 60;
    int viewHeight = 25;
    int scrollStep = 5;
};

inline AppConfig loadConfig(const std::string& path = "config.json") {
    AppConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Config file '" << path << "' not found, using defaults.\n";
        return config;
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Config file '" << path << "' is not valid JSON (" << e.what()
                   << "), using defaults.\n";
        return config;
    }

    config.host = json.value("host", config.host);
    config.port = json.value("port", config.port);

    if (json.contains("area") && json["area"].is_object()) {
        config.areaWidth = json["area"].value("width", config.areaWidth);
        config.areaHeight = json["area"].value("height", config.areaHeight);
    }

    config.boulderCount = json.value("boulder_count", config.boulderCount);
    config.boulderSeed = json.value("boulder_seed", config.boulderSeed);

    config.tickIntervalMs = json.value("tick_interval_ms", config.tickIntervalMs);
    config.tickCount = json.value("tick_count", config.tickCount);

    if (json.contains("view") && json["view"].is_object()) {
        config.viewWidth = json["view"].value("width", config.viewWidth);
        config.viewHeight = json["view"].value("height", config.viewHeight);
    }
    config.scrollStep = json.value("scroll_step", config.scrollStep);

    return config;
}

} // namespace goblins

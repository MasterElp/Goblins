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

// Все структуры конфигурации читаются через
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT, а не через строгий
// вариант: отсутствующее поле берёт значение по умолчанию вместо того,
// чтобы уронить разбор всего файла. Иначе добавление любого нового поля
// (например, saves_dir) превращало бы уже существующие config.json в
// "невалидные" целиком — со сбросом на умолчания вообще всех настроек,
// включая host/port. Конфигурация должна быть удобной, а не
// обязательной — тот же принцип, что и у ensureConfigExists ниже.

struct AreaSize {
    int width = 100;
    int height = 100;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AreaSize, width, height)

// Зеркало core::TerrainParams (core/generation/TerrainParams.hpp) — но
// JSON-сериализуемое. Дублирование полей осознанное: core не знает о
// JSON и конфигурации вообще (07_TechStack.md, п.6), поэтому именно
// server (main.cpp) переносит значения из этой структуры в
// core::TerrainParams перед вызовом generateTerrain. Имена и значения по
// умолчанию должны совпадать с core::TerrainParams — включая и то, каких
// полей здесь НЕТ: числа, одинаковые для любого мира (форма fBm, уклон
// дна русла, испарение, сток за край карты, пороги), живут константами
// рядом с местом использования в core и не тянутся через конфигурацию,
// сеть и файл сохранения ради того, чтобы никогда не быть изменёнными.
struct TerrainConfig {
    // Масштаб рельефа. Частоты остальных слоёв шума (каменистость,
    // утрамбованность, минералы) — фиксированные кратные от неё, см.
    // core::TerrainParams::noiseFrequency.
    float noise_frequency = 0.02f;
    int noise_octaves = 4;

    // Насколько твёрдая земля (каменистость и утрамбованность вместе)
    // поднимает рельеф — вода такие участки огибает.
    float hardness_height_bump = 0.6f;

    int river_count = 3;
    float river_width = 3.0f;
    float river_sinuosity = 0.5f;
    float river_depth = 0.9f;

    // Средняя глубина воды пруда — в тех же единицах, что и river_depth
    // (глубина тайла, не множитель).
    float pond_depth = 0.9f;

    // Минералы (SoilComponent.minerals): среднее по карте значение при
    // генерации — см. core::TerrainParams::mineralsAverage.
    float minerals_average = 10.0f;

    // Источники воды (WaterSourceComponent): сколько "родников" в
    // случайных точках карты (плюс автоматически — по одному на исток
    // каждой реки). water_source_strength — абсолютный приток (глубина за
    // тик) и свойство мира (core::WorldPropertiesComponent), не текущий
    // параметр симуляции: генерация выбирает его один раз, во время самой
    // симуляции он не меняется.
    int water_source_count = 3;
    float water_source_strength = 0.05f;

    // Отток воды и дожди — вторая половина баланса воды, см.
    // core::TerrainParams::waterEvaporationRate.
    float water_evaporation_rate = 0.0002f;
    int rain_interval_ticks = 400;
    float rain_amount = 0.05f;

    // Доля разницы уровней поверхности, перетекающая к самому низкому
    // соседу за тик на ровном месте — тоже свойство мира, см.
    // core::TerrainParams::waterFlowRate.
    float water_flow_rate = 0.3f;

    // Эрозия: скорость вымывания породы потоком и потолок выемки
    // относительно соседа — тоже свойства мира, см.
    // core::TerrainParams::soilErosionRate/maxErosionDepth.
    float soil_erosion_rate = 0.05f;
    float max_erosion_depth = 0.5f;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TerrainConfig, noise_frequency, noise_octaves, hardness_height_bump,
                                    river_count, river_width, river_sinuosity, river_depth, pond_depth,
                                    minerals_average, water_source_count, water_source_strength,
                                    water_evaporation_rate, rain_interval_ticks, rain_amount, water_flow_rate,
                                    soil_erosion_rate, max_erosion_depth)

// Зеркало core::PlantParams (core/generation/PlantParams.hpp) — по той же
// причине, что и TerrainConfig выше: core не знает о JSON, поэтому
// перенос значений делает server (main.cpp). Имена и значения по
// умолчанию должны совпадать с core::PlantParams.
struct PlantConfig {
    // Сколько видов травы в мире. Ядро обрежет значение до 3..12
    // (core::kMinGrassSpecies/kMaxGrassSpecies): меньше трёх — не о чем
    // говорить, больше двенадцати — виды перестают быть различимыми при
    // одном и том же бюджете преимуществ.
    int grass_species = 5;

    // Какая доля клеток засевается при генерации. Дальше трава
    // расселяется сама (PlantSystem), поэтому это стартовая
    // заселённость, а не итоговая.
    float grass_coverage = 0.06f;

    // Свойства мира (core::WorldPropertiesComponent), как и
    // water_flow_rate у террейна: выбираются при генерации, во время
    // симуляции PlantSystem их только читает.
    float mutation_rate = 0.06f;
    float humus_decay_rate = 0.02f;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlantConfig, grass_species, grass_coverage, mutation_rate,
                                    humus_decay_rate)

struct ServerConfig {
    std::string host = "127.0.0.1";
    int port = 9002;

    AreaSize area{};

    // Один seed на весь мир: террейн, булыжники и растения раньше крутили
    // независимые seed'ы, но это давало три ползунка, которые меняли одно
    // и то же — "другой мир" — и запутывали (какой из трёх крутить, чтобы
    // получить другую карту?). Внутри генерации каждая стадия по-прежнему
    // получает свой собственный поток случайности — не буквально это
    // число, а с постоянным смещением от него (см. kBoulderSeedOffset и
    // kPlantSeedOffset в server/main.cpp, и seed/seed+1/seed+2/seed+10/
    // seed+20 внутри generateTerrain) — иначе одинаковый seed давал бы
    // подозрительно похожие узоры камней и травы у разных миров.
    unsigned seed = 54321;
    TerrainConfig terrain{};

    int boulder_count = 40;

    PlantConfig plants{};

    // Целевой интервал тика. Сколько тиков выполнить — не настройка:
    // мир существует независимо от наблюдателя и тикает, пока сервер жив
    // (02_CorePrinciples.md, п.1); прежний tick_count всегда стоял в -1
    // ("бесконечно") и был отладочным ограничителем, а не параметром
    // мира.
    int tick_interval_ms = 200;

    // Как часто состояние мира уходит клиентам. Это не тот же интервал,
    // что tick_interval_ms: мир может тикать чаще, чем имеет смысл
    // рисовать, и тогда рассылка каждый тик только копит очередь в
    // сокете — клиент показывал бы прошлое (тик продолжал бы расти уже
    // после паузы). Ноль — рассылать на каждом тике.
    int snapshot_interval_ms = 100;

    // Каталог сохранённых миров (по одному JSON-файлу на мир, см.
    // server/WorldSave.hpp). Относительный путь разрешается относительно
    // каталога исполняемого файла сервера, а не рабочей директории.
    std::string saves_dir = "saves";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ServerConfig, host, port, area, seed, terrain, boulder_count,
                                    plants, tick_interval_ms, snapshot_interval_ms,
                                    saves_dir)

// Подмножество ServerConfig, которое можно перегенерировать вживую по
// сети (протокол "regenerate", см. NetworkServer.hpp) без пересоздания
// World: размер Области — нет, она фиксирована при создании World и
// затрагивает GameLoop/NetworkServer, которые держат на неё ссылку;
// террейн и булыжники — да, это просто новый набор Entity на том же
// Area.
struct RegenerationRequest {
    unsigned seed = 54321;
    TerrainConfig terrain{};
    int boulder_count = 40;
    PlantConfig plants{};
};
// ..._WITH_DEFAULT, а не строгий вариант, — по той же причине, что и у
// структур конфигурации выше: этот запрос лежит внутри каждого файла
// сохранённого мира ("generation"), и со строгим разбором добавление
// любого нового поля (например, растений) делало бы все ранее
// сохранённые миры нечитаемыми целиком.
//
// terrain_seed/boulder_seed/plant_seed из старых файлов сохранений здесь
// не читаются намеренно: у мира, сгенерированного до объединения seed'ов,
// они всё равно втроём не сойдутся обратно в одно число — WITH_DEFAULT
// в этом случае подставит seed=54321, и старый мир на диске (сами Entity,
// не "generation") от этого не пострадает, только надпись "чем
// сгенерирован" в панели будет неточной для таких файлов.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RegenerationRequest, seed, terrain, boulder_count, plants)

struct ClientConfig {
    std::string host = "127.0.0.1";
    int port = 9002;

    // Размер тайла в пикселях на экране (режим "Симуляция"; в режиме
    // "Генерация мира" тайл всегда пересчитывается так, чтобы вся карта
    // помещалась на экране, это поле там не используется).
    int tile_size = 16;

    // Графика — экран "Настройки". Сохраняется между запусками.
    int window_width = 1280;
    int window_height = 720;
    bool fullscreen = false;

    // Экран мира: какие слои почвы включены, во сколько раз увеличена
    // карта и открыта ли панель параметров генерации — сохраняется между
    // запусками клиента, как и графика выше, чтобы каждый раз не
    // подбирать заново одно и то же (например, "минералы выключены,
    // потому что мешают смотреть на растения"). Позиция прокрутки
    // (viewX/viewY) НЕ сохраняется — осмысленной точкой отсчёта она была
    // бы только для конкретного мира, а после запуска клиента мир может
    // оказаться другим.
    bool show_rockiness = true;
    bool show_compaction = true;
    bool show_moisture = true;
    bool show_minerals = true;
    bool show_height = true;
    bool show_plants = true;
    float zoom = 1.0f;

    // Панель параметров генерации на экране мира. По умолчанию закрыта:
    // 460px справа нужны, только когда параметры действительно крутят.
    bool show_generation_panel = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ClientConfig, host, port, tile_size, window_width, window_height,
                                    fullscreen, show_rockiness, show_compaction, show_moisture, show_minerals,
                                    show_height, show_plants, zoom, show_generation_panel)

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

// Явное сохранение — используется экраном "Настройки" после применения
// графических параметров, чтобы они не сбрасывались при следующем
// запуске.
inline void saveClientConfig(const std::string& path, const ClientConfig& config) {
    detail::writeConfigFile(path, config);
}

// Явное сохранение — сервер вызывает это по запросу клиента ("Save
// values" на панели генерации), чтобы текущие параметры генерации стали
// значениями по умолчанию при следующем запуске сервера.
inline void saveServerConfig(const std::string& path, const ServerConfig& config) {
    detail::writeConfigFile(path, config);
}

// Путь к config.json рядом с текущим исполняемым файлом — конфигурация
// генерируется и живёт там же, где сам бинарник, а не в рабочей
// директории, из которой его запустили.
inline std::string defaultConfigPathNextToExecutable() {
    return (getExecutableDirectory() / "config.json").string();
}

} // namespace goblins

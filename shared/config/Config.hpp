#pragma once

#include <algorithm>
#include <cmath>
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

// Пределы стороны Области. Общие для клиента (ползунки размера на панели
// генерации) и сервера (проверка присланного запроса): расходись они, и
// клиент предлагал бы размер, который сервер молча урежет.
//
// Нижняя граница — чтобы в мире вообще уместились река, пруд и стадо;
// верхняя — цена полного снапшота: в world_init уходит десяток плотных
// массивов на всю Область (512x512 — это уже больше двух миллионов чисел),
// и дальше сообщение перестаёт помещаться в разумное время отправки.
inline constexpr int kMinAreaSide = 32;
inline constexpr int kMaxAreaSide = 512;

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
// Все числа целые — как и всё состояние мира (core/Scale.hpp). Доли
// (каменистость гор, извилистость) — в тысячных, высоты и глубины — в
// тысячных единицы глубины, ширина реки — в десятых долях тайла. Дробных
// ползунков в панели генерации больше нет ни одного.
struct TerrainConfig {
    // Размер узора рельефа в тайлах: во сколько тайлов укладывается одна
    // крупная форма. Масштабы остальных слоёв шума (каменистость,
    // минералы) — фиксированные доли от него, см.
    // core::TerrainParams::featureSize.
    int feature_size = 50;
    int noise_octaves = 4;

    // Высота высочайшей вершины мира — в тех же единицах, что глубина воды
    // (тысячные, см. core::TerrainParams::mountainHeight).
    int mountain_height = 16000;

    // Насколько высота рельефа делает почву каменистой (в тысячных): горы
    // и их подножия сложены камнем. Связь именно такая, а не обратная
    // ("камень поднимает рельеф", как было раньше) — см.
    // core::TerrainParams::mountainHardness.
    int mountain_hardness = 600;

    int river_count = 3;
    // В десятых долях тайла: 30 — это три тайла.
    int river_width = 30;
    // В тысячных: 0 — прямая линия, 1000 — максимальный меандр.
    int river_sinuosity = 500;
    int river_depth = 900;

    // Средняя глубина воды пруда — в тех же единицах, что и river_depth
    // (глубина тайла, не множитель).
    int pond_depth = 900;

    // Минералы (SoilComponent.minerals): среднее по карте значение при
    // генерации. Крупицы счётные, шкалы у них нет.
    int minerals_average = 10;

    // Разносит ли течение воды минералы между тайлами (см.
    // core::WorldPropertiesComponent::mineralsSpreadEnabled) — свойство
    // мира, читается каждый тик, а не только при генерации.
    bool minerals_spread_enabled = true;

    // Источники воды (WaterSourceComponent): сколько "родников" в
    // случайных точках карты (плюс автоматически — ровно один на исток
    // каждой реки). water_source_depth — глубина СТОЛБА воды источника и
    // свойство мира (core::WorldPropertiesComponent), не текущий параметр
    // симуляции: источник всегда стоит на этой глубине, сколько бы из него
    // ни вытекло, поэтому мир заливает не глубина столба, а число
    // источников.
    int water_source_count = 3;
    int water_source_depth = 1000;

    // Отток воды и дожди — вторая половина баланса воды. Испарение — в
    // тысячных глубины за сто тиков (см.
    // core::TerrainParams::waterEvaporationRate).
    int water_evaporation_rate = 20;
    int rain_interval_ticks = 400;
    int rain_amount = 50;

    // Эрозия: скорость вымывания породы потоком (в тысячных долях) — тоже
    // свойство мира, см. core::TerrainParams::soilErosionRate.
    int soil_erosion_rate = 50;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TerrainConfig, feature_size, noise_octaves, mountain_height,
                                    mountain_hardness, river_count, river_width, river_sinuosity, river_depth, pond_depth,
                                    minerals_average, minerals_spread_enabled, water_source_count, water_source_depth,
                                    water_evaporation_rate, rain_interval_ticks, rain_amount,
                                    soil_erosion_rate)

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
    // В тысячных долях карты: 60 — это прежние 6%.
    int grass_coverage = 60;

    // Свойства мира (core::WorldPropertiesComponent), как и
    // water_flow_rate у террейна: выбираются при генерации, во время
    // симуляции PlantSystem их только читает.
    // В тысячных долях вложения черты: 60 — это прежние 6%.
    int mutation_rate = 60;

    // Раз во сколько тиков перегной возвращает в почву одну крупицу
    // минералов — срок, а не дробная скорость (см.
    // core::PlantParams::humusDecayPeriod).
    int humus_decay_period = 50;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlantConfig, grass_species, grass_coverage, mutation_rate,
                                    humus_decay_period)

// Зеркало core::AnimalParams (core/generation/AnimalParams.hpp) — по той же
// причине, что TerrainConfig и PlantConfig выше. Имена и значения по
// умолчанию должны совпадать с core::AnimalParams.
//
// Травоядные и хищники — одна секция, а не две: они появляются одним этапом
// генерации и живут по одной системе, различаясь только диетой.
struct AnimalConfig {
    // Сколько видов каждой диеты в мире. Ядро обрежет значения к своим
    // границам (1..8 травоядных, 1..6 хищников).
    int herbivore_species = 2;
    int predator_species = 1;

    // Сколько особей выпускается при генерации — штуки, а не доля карты:
    // животных десятки, а не тысячи. Хищников заметно меньше, чем добычи:
    // иначе они выедят её раньше, чем она успеет расплодиться. Ноль в любой
    // строке — мир без этого звена.
    int herbivore_count = 120;
    int predator_count = 10;

    // Свойство мира (core::WorldPropertiesComponent): выбирается при
    // генерации, во время симуляции AnimalSystem его только читает. Одно на
    // всех животных — это скорость наследственных изменений в этом мире, а
    // не свойство диеты. В тысячных долях вложения черты, как и у растений.
    int mutation_rate = 60;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AnimalConfig, herbivore_species, predator_species,
                                    herbivore_count, predator_count, mutation_rate)

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
    AnimalConfig animals{};

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
                                    plants, animals, tick_interval_ms, snapshot_interval_ms,
                                    saves_dir)

// Подмножество ServerConfig, которое можно перегенерировать вживую по
// сети (протокол "regenerate", см. NetworkServer.hpp). Размер Области
// сюда входит наравне с остальным: World::reset пересоздаёт Область на
// месте, а GameLoop и NetworkServer держат ссылку на сам World, а не на
// его Область, — тем же путём в мир приходит и загруженное сохранение
// другого размера. Раньше размер был исключением ("фиксирован при
// создании World"), и сменить его можно было только перезапуском сервера
// с другим area в config.json.
struct RegenerationRequest {
    // Размер Области нового мира, в тайлах. Ядро ничего не знает об этих
    // границах — проверяет их сервер при разборе запроса (kMinAreaSide /
    // kMaxAreaSide выше).
    int area_width = 100;
    int area_height = 100;

    unsigned seed = 54321;
    TerrainConfig terrain{};
    int boulder_count = 40;
    PlantConfig plants{};
    AnimalConfig animals{};
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
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RegenerationRequest, area_width, area_height, seed, terrain,
                                    boulder_count, plants, animals)

// --- Перекладывание полей между ServerConfig и RegenerationRequest ---
//
// Два места, и оба здесь, вплотную к самим структурам. Раньше их было три,
// и все три — руками, в разных файлах: сервер собирал запрос из
// конфигурации при старте (server/main.cpp), а обработчик "Save values"
// раскладывал запрос обратно (server/NetworkServer.cpp). Кто добавлял
// новую секцию, правил ту из трёх, которую вспомнил.
//
// Так и вышло с животными: секция появилась в конфигурации, в панели и в
// протоколе, но ни в сборку запроса, ни в сохранение её не дописали.
// Настройки поголовья не доезжали ни туда, ни обратно — панель показывала
// умолчания, мир рождался с умолчаниями, а "Save values" молча выбрасывал
// правки. Ровно тем же кончилась и предыдущая попытка ("Не сохранялись
// параметры" в CHANGELOG): три списка полей нельзя удержать в согласии,
// их нужно свести в один.
//
// Теперь список один и лежит там, где новое поле и добавляют, — рядом со
// структурами. Пропустить его, дописывая параметр, всё ещё можно, зато
// теперь для этого надо не заметить соседнюю функцию, а не вспомнить про
// три файла.
inline RegenerationRequest toRegenerationRequest(const ServerConfig& config) {
    RegenerationRequest request;
    request.area_width = config.area.width;
    request.area_height = config.area.height;
    request.seed = config.seed;
    request.terrain = config.terrain;
    request.boulder_count = config.boulder_count;
    request.plants = config.plants;
    request.animals = config.animals;
    return request;
}

// Обратно — в уже существующий ServerConfig: настройки, не относящиеся к
// генерации (host, port, saves_dir, интервалы тика и снапшота), у него
// свои и правятся не с панели, поэтому конфигурация не собирается заново,
// а дополняется.
//
// Размер Области подрезается к границам здесь же: он приходит от клиента,
// то есть снаружи, и класть в файл значение, с которым мир не
// сгенерируется, нельзя.
inline void applyGeneration(ServerConfig& config, const RegenerationRequest& request) {
    config.area.width = std::clamp(request.area_width, kMinAreaSide, kMaxAreaSide);
    config.area.height = std::clamp(request.area_height, kMinAreaSide, kMaxAreaSide);
    config.seed = request.seed;
    config.terrain = request.terrain;
    config.boulder_count = request.boulder_count;
    config.plants = request.plants;
    config.animals = request.animals;
}

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
    bool show_moisture = true;
    bool show_minerals = true;
    bool show_height = true;
    bool show_plants = true;
    bool show_animals = true;
    // Встревоженность земли — след недавней охоты (DangerComponent).
    // Отдельный выключатель, а не часть слоя животных: это состояние
    // почвы, и смотреть на него обычно нужно как раз тогда, когда
    // разбираешься, почему стадо ушло с этого края карты.
    bool show_danger = true;
    float zoom = 1.0f;

    // Какая вкладка открыта в правой панели экрана мира: "info"
    // (карточка существа или клетки), "params" (параметры генерации),
    // "graphs" (численность по времени), "genome" (средний геном по
    // времени), "keys" (управление) или
    // "hidden" — панель свёрнута и карта занимает всё окно. Одно поле на
    // все панели, а не выключатель на каждую: они и на экране занимают
    // одно и то же место, показывать их одновременно негде. Строкой, а не
    // числом, чтобы файл конфигурации оставался читаемым глазами.
    //
    // По умолчанию свёрнута: 460px справа нужны, только когда в них
    // действительно смотрят. Содержимое вкладок здесь не хранится —
    // летопись численности ведёт сервер (server/PopulationHistory.hpp), а
    // параметры генерации приходят от него же.
    std::string panel_tab = "hidden";
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ClientConfig, host, port, tile_size, window_width, window_height,
                                    fullscreen, show_rockiness, show_moisture, show_minerals,
                                    show_height, show_plants, show_animals, show_danger, zoom, panel_tab)

namespace detail {

// Первое дробное число, найденное где-нибудь в дереве JSON, — вместе с
// путём до него. Пустая строка, если дробных чисел нет.
//
// Нужно ровно для одного случая, зато важного. Все настройки сервера стали
// целыми (core/Scale.hpp), и числа при этом сменили не только тип, но и
// СМЫСЛ: 0.06 доли карты — это теперь 60 тысячных, 29.53 высоты — 29530.
// nlohmann, встретив дробное значение в int-поле, не ругается, а отбрасывает
// дробную часть: "grass_coverage": 0.06 молча превращается в 0, то есть в
// мир вообще без травы, а "mountain_height": 29.53 — в плоскую карту. И
// того, и другого пользователь не увидит никак: мир просто выйдет пустым.
//
// Ловим именно дробность, а не имена ключей: часть имён (например,
// water_evaporation_rate) уцелела при смене смысла, а часть настроек
// перестала быть дробной ещё раньше. Дробное число в конфигурации сервера
// не имеет права встретиться ни одно — значит его наличие и есть надёжный
// признак файла старой формы.
inline std::string findFractional(const nlohmann::json& json, const std::string& path = "") {
    if (json.is_number_float()) {
        const double value = json.get<double>();
        if (value != std::floor(value)) {
            return path.empty() ? "<root>" : path;
        }
        return "";
    }
    if (json.is_object()) {
        for (const auto& [key, value] : json.items()) {
            const std::string found = findFractional(value, path.empty() ? key : path + "." + key);
            if (!found.empty()) {
                return found;
            }
        }
    }
    return "";
}

// hasIntegerScale — есть ли у этой конфигурации целая шкала (у серверной
// есть, у клиентской нет: там живёт дробный zoom). Проверка старой формы
// имеет смысл только для первой.
template <typename Config>
Config loadConfigFile(const std::string& path, bool hasIntegerScale) {
    Config config;

    std::ifstream file(path);
    if (!file.is_open()) {
        return config;
    }

    try {
        nlohmann::json json;
        file >> json;

        if (hasIntegerScale) {
            const std::string fractional = findFractional(json);
            if (!fractional.empty()) {
                std::cerr << "Config file '" << path << "' is from an older version: it holds a fractional value at '"
                          << fractional << "', and settings are whole numbers now.\n"
                          << "  The numbers changed meaning as well as type: fractions became thousandths\n"
                          << "  (0.06 of the map -> 60), depths and heights too (0.9 deep -> 900).\n"
                          << "  Reading it as is would silently give you an empty, flat world, so defaults\n"
                          << "  are used instead. Delete the file to get a documented one, or convert it:\n"
                          << "  noise_frequency 0.02 -> feature_size 50, humus_decay_rate 0.02 ->\n"
                          << "  humus_decay_period 50, water_evaporation_rate 0.0002 -> 20 (per 100 ticks).\n";
                return config;
            }
        }

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
    // true: у настроек мира целая шкала целиком, дробному числу тут взяться
    // неоткуда — см. detail::findFractional.
    return detail::loadConfigFile<ServerConfig>(path, true);
}

inline ClientConfig loadClientConfig(const std::string& path) {
    // false: у клиента остался дробный zoom — это не состояние мира, а
    // масштаб картинки на экране.
    return detail::loadConfigFile<ClientConfig>(path, false);
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

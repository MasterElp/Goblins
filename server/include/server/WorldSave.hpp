#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.hpp"
#include "core/World.hpp"
#include "server/PopulationHistory.hpp"
#include "world/WorldSaveInfo.hpp"

namespace goblins {

// Сохранение и загрузка миров. Живёт в server, а не в core: core не
// знает ни о файлах, ни о JSON (07_TechStack.md, п.6) — он отдаёт только
// состояние мира через публичный интерфейс World, а как именно оно
// ложится на диск, решает слой вокруг ядра. Поэтому перенос
// "компоненты <-> JSON" здесь явный, ровно как перенос
// TerrainConfig -> TerrainParams в server/main.cpp.
//
// Один мир — один файл `<имя>.json` в каталоге сохранений. Имя файла
// (без расширения) и есть идентификатор мира: так список миров не
// зависит от содержимого файлов, а два мира не могут случайно оказаться
// с одним именем.
//
// Формат файла (версия 1, dump без отступов — это машинные данные, а не
// конфигурация для правки руками):
//   {"format": "goblins_world", "version": 1,
//    "info": WorldSaveInfo,
//    "generation": RegenerationRequest,
//    "history": {"interval": N,
//                "points": [[тик, [трава по видам], [животные по видам]], ...]},
//    "entities": [ {"position": {"x","y"},
//                   "soil": {"moisture","rockiness","compaction","minerals"},
//                   "height": H,
//                   "water": {"depth"},
//                   "water_source": true,
//                   "humus": {"minerals","pending"},
//                   "impassable": true,
//                   "plant": {"age","growth","moisture","minerals",
//                              "mineral_pending","stress"},
//                   "genome": {"species": N, "<черта>": V, ...},
//                   "herbivore": {"age","growth","sex","energy","water",
//                                  "protein","protein_pending","dung",
//                                  "dung_pending","step_progress","stress"},
//                   "herbivore_genome": {"species": N, "<черта>": V, ...},
//                   "desire": {"hunger","thirst","mating","current"},
//                   "identity": N,
//                   "time": {"tick": N},
//                   "world_properties": {"water_source_depth": S,
//                                         "water_evaporation_rate": R,
//                                         "rain_interval_ticks": RI,
//                                         "rain_amount": RA,
//                                         "soil_erosion_rate": E,
//                                         "max_erosion_depth": D,
//                                         "plant_mutation_rate": M,
//                                         "humus_decay_rate": H,
//                                         "plant_random_seed": P,
//                                         "animal_mutation_rate": AM,
//                                         "animal_random_seed": AS},
//                   "plant_species": [ {"species": 0, "<черта>": V, ...}, ... ],
//                   "herbivore_species": [ {"species": 0, "<черта>": V, ...}, ... ]}, ... ]}
//
// "soil.minerals" (SoilComponent.minerals, целое число) — как и "height",
// добавлено без смены версии: у старых файлов без этого поля минералы
// читаются как 0.
//
// "world_properties" (WorldPropertiesComponent, живёт на том же World
// Entity, что и "time" — 06_GameLoop.md, п.1a) — свойства мира, выбранные
// один раз при генерации и не меняющиеся во время симуляции. У старых
// файлов без этого поля (или без отдельных полей внутри него) действуют
// значения по умолчанию (water_source_depth = 1, water_evaporation_rate =
// 0.0002, rain_interval_ticks = 400, rain_amount = 0.05,
// soil_erosion_rate = 0.05, max_erosion_depth = 0.5,
// plant_mutation_rate = 0.06, humus_decay_rate = 0.02,
// animal_mutation_rate = 0.06) — World::reset выставляет их сам.
//
// "water_source" (WaterSourceComponent) — тег, как "impassable": сам
// факт наличия и есть данные, отсутствие поля у старых файлов означает
// "не источник" (02_CorePrinciples.md, п.3).
//
// "height" (HeightComponent) добавлено без смены версии формата: старые
// файлы без этого поля читаются как есть (высота считается 0 — плоский
// рельеф), HydrologySystem корректно работает и без начального градиента.
//
// "plant"/"genome" (живое растение), "humus" (HumusComponent — лежит на
// том же Entity тайла, что почва и вода) и "plant_species" (виды травы
// этого мира, на World Entity) — тоже без смены версии: в мире из старого
// файла растений просто нет. Черты генома пишутся по именам из таблицы
// core::kGrassTraits, а не фиксированным списком полей, поэтому новая
// черта попадает в файл сама, а её отсутствие в старом файле означает
// значение по умолчанию, а не ошибку разбора. Растение без генома —
// ошибка: подставить "средний геном" значило бы втихую изменить
// состояние мира при загрузке.
//
// "herbivore"/"herbivore_genome"/"desire"/"identity" (живое травоядное) и
// "herbivore_species" (виды травоядных этого мира, на World Entity) —
// добавлены без смены версии формата по той же причине: в мире из старого
// файла животных просто нет, и он открывается как открывался. Черты генома
// пишутся по именам из таблицы core::kHerbivoreTraits — тем же общим кодом,
// что и черты травы. Животное без генома — ошибка, как и растение;
// животное без "identity" — не ошибка: постоянный идентификатор ему
// выдаётся заново (он всего лишь ключ случайности, а мир недетерминирован,
// 02_CorePrinciples.md, п.12a).
//
// "history" (server/PopulationHistory.hpp) — летопись численности видов,
// добавлена без смены версии формата: у мира из старого файла её просто
// нет, и она начинается заново с момента загрузки. Это единственная часть
// файла, которая не является состоянием мира: сам мир от неё не зависит и
// без неё живёт ровно так же — но потерять её вместе с закрытием сервера
// значило бы, что смотреть на длинную жизнь мира негде.
//
// Сохраняется полное состояние мира — все Entity со всеми компонентами,
// включая World Entity с TimeComponent (запись с ключом "time"), а не
// только seed'ы генерации: мир после N тиков уже не восстанавливается
// повторной генерацией, поэтому "сохранить seed" сохранением мира не
// является. Индекс размещения Area в файл не пишется — он полностью
// выводится из PositionComponent/ImpassableComponent и перестраивается
// при загрузке.
inline constexpr int kWorldSaveFormatVersion = 1;

// Каталог сохранений. Относительный путь из конфигурации разрешается
// относительно каталога исполняемого файла (как и config.json клиента),
// а не рабочей директории, из которой сервер запустили.
std::filesystem::path resolveSaveDirectory(const std::string& configured);

// Имя мира приходит по сети, а превращается в путь на диске — поэтому
// проверяется явно: только буквы/цифры/`-`/`_`/`.`/пробел, без
// разделителей пути и без ".." (иначе load_world мог бы прочитать любой
// файл на машине сервера).
bool isValidWorldName(const std::string& name);

// Имя для нового мира: `world-ГГГГММДД-ЧЧММСС` (UTC), с числовым
// суффиксом, если такое имя уже занято.
std::string makeUniqueWorldName(const std::filesystem::path& directory);

// Заголовки всех сохранённых миров, свежие сверху (по saved_at).
// Файлы, которые не читаются или не являются сохранением мира,
// пропускаются с сообщением в stderr — один сломанный файл не должен
// прятать остальные миры.
std::vector<WorldSaveInfo> listWorldSaves(const std::filesystem::path& directory);

// Записывает текущее состояние world в `<directory>/<name>.json`.
// Читает ECS registry, поэтому вызывать только с потока GameLoop (как и
// регенерацию — см. NetworkServer::takePendingRegeneration).
//
// Запись атомарная (через временный файл + rename): прерванное
// сохранение не портит предыдущую версию мира. created_at существующего
// файла сохраняется — мир "создан" один раз, а сохраняется многократно.
bool saveWorld(const World& world, const RegenerationRequest& generation, const PopulationHistory& history,
               const std::string& name, const std::filesystem::path& directory, WorldSaveInfo& outInfo,
               std::string& outError);

// Загружает мир из `<directory>/<name>.json`: сбрасывает world (включая
// размер Области — у сохранения он свой) и заполняет его заново.
// Так же, как saveWorld, трогает ECS registry — только с потока GameLoop.
//
// Файл сначала полностью разбирается и проверяется и только потом
// применяется к world: при ошибке (битый файл, координаты вне Области)
// мир остаётся нетронутым, а не наполовину загруженным.
bool loadWorld(World& world, const std::string& name, const std::filesystem::path& directory,
               RegenerationRequest& outGeneration, PopulationHistory& outHistory, WorldSaveInfo& outInfo,
               std::string& outError);

// Удаляет `<directory>/<name>.json`. Только файловый ввод-вывод, ECS
// registry не трогает — в отличие от saveWorld/loadWorld, можно вызывать
// прямо с сетевого потока (как list_worlds).
bool deleteWorld(const std::string& name, const std::filesystem::path& directory, std::string& outError);

} // namespace goblins

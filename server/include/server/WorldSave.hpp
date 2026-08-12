#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.hpp"
#include "core/World.hpp"
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
//    "entities": [ {"position": {"x","y"},
//                   "soil": {"moisture","rockiness","compaction"},
//                   "water": {"depth","flow_speed"},
//                   "impassable": true,
//                   "time": {"tick": N}}, ... ]}
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
bool saveWorld(const World& world, const RegenerationRequest& generation, const std::string& name,
               const std::filesystem::path& directory, WorldSaveInfo& outInfo, std::string& outError);

// Загружает мир из `<directory>/<name>.json`: сбрасывает world (включая
// размер Области — у сохранения он свой) и заполняет его заново.
// Так же, как saveWorld, трогает ECS registry — только с потока GameLoop.
//
// Файл сначала полностью разбирается и проверяется и только потом
// применяется к world: при ошибке (битый файл, координаты вне Области)
// мир остаётся нетронутым, а не наполовину загруженным.
bool loadWorld(World& world, const std::string& name, const std::filesystem::path& directory,
               RegenerationRequest& outGeneration, WorldSaveInfo& outInfo, std::string& outError);

} // namespace goblins

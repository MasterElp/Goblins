#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace goblins {

// Краткое описание сохранённого мира — заголовок файла сохранения и
// одновременно элемент списка миров в протоколе ("world_list", см.
// server/NetworkServer.hpp). Лежит в shared/, а не в server/, ровно по
// той же причине, что и структуры конфигурации: это форма данных,
// которую обе стороны протокола обязаны понимать одинаково, а клиент
// заголовков server/ не видит (07_TechStack.md, п.6 — клиент получает
// данные только по протоколу).
//
// Само состояние мира (Entity с компонентами) сюда не входит: список
// миров в меню должен быть дешёвым, а полное состояние читает только
// сервер и только при загрузке конкретного мира.
struct WorldSaveInfo {
    // Имя мира = имя файла сохранения без расширения. Оно же —
    // идентификатор мира в протоколе (клиент присылает его в
    // start_simulation).
    std::string name;

    // Номер тика, на котором мир сохранён. 0 — мир сохранён сразу после
    // генерации, до первого тика.
    std::uint64_t tick = 0;

    int area_width = 0;
    int area_height = 0;

    unsigned seed = 0;

    // UTC, ISO-8601. created_at — момент первого сохранения мира (то
    // есть его генерации), saved_at — момент последней записи файла.
    std::string created_at;
    std::string saved_at;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WorldSaveInfo, name, tick, area_width, area_height, seed,
                                    created_at, saved_at)

} // namespace goblins

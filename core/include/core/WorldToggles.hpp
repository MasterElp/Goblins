#pragma once

namespace goblins {

// Отключаемые механики мира — те, что раньше либо не выключались вовсе,
// либо требовали отключать закон целиком. Одна структура на все такие
// переключатели, а не булево поле россыпью в TerrainParams и
// WorldPropertiesComponent: обе структуры несут её ЦЕЛИКОМ одним полем
// (см. TerrainParams::toggles, WorldPropertiesComponent::toggles), и
// перенос между ними (TerrainGenerator.cpp) — одна строка присваивания,
// а не отдельный проброс на каждый новый переключатель.
//
// Добавляя новый выключатель: поле сюда, зеркальное поле в
// shared::TerrainToggles (shared/config/Config.hpp, у него своя копия —
// core не знает о JSON, 07_TechStack.md, п.6), одна строка в
// server/main.cpp::toTerrainParams (посл. этого файла нет способа
// перенести структуру одной строкой — типы разные), строка в
// tools/check_animal... нет, в tools/check_config_roundtrip.py PROBE, и
// одна запись в kToggleRows (clients/desktop/src/SettingsPanel.cpp) —
// панель рисует чекбоксы из этого списка, а не отдельным вызовом на
// каждый. Системы (HydrologySystem и далее) читают поле по имени там,
// где решают, применять ли отключаемый эффект.
struct WorldToggles {
    // Разносит ли течение воды минералы между тайлами (правило песочной
    // кучи, HydrologySystem). Выключенное не трогает сами крупицы — их
    // по-прежнему добавляет и забирает перегной (humusDecayPeriod).
    bool mineralsSpread = true;

    // Оседает ли вымытая эрозией порода в углублениях ниже по течению
    // (HydrologySystem). Выключенное не останавливает саму эрозию (её
    // выключает soilErosionRate = 0) — вымытое просто всегда уходит с
    // карты целиком, как вода за край, вместо того чтобы копиться в
    // низинах.
    bool erosionDeposition = true;
};

} // namespace goblins

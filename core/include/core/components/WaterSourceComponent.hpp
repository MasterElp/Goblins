#pragma once

namespace goblins {

// Тег-компонент: тайл — постоянный источник воды (исток реки или
// "родник" в случайной точке карты), см. TerrainGenerator.cpp. Как и
// ImpassableComponent — без данных, само наличие компонента и есть факт
// (02_CorePrinciples.md, п.3: отсутствие компонента = отсутствие
// возможности). HydrologySystem каждый тик подпитывает такие тайлы, не
// давая им пересохнуть от испарения (WorldPropertiesComponent.
// waterEvaporationRate) — компонент выставляется один раз при генерации
// и System-ами далее не удаляется/не добавляется.
struct WaterSourceComponent {};

} // namespace goblins

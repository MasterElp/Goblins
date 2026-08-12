#pragma once

#include "core/World.hpp"
#include "core/generation/TerrainParams.hpp"

namespace goblins {

// Шаг процедурной генерации мира — формирование почвы и водоёмов
// (02_CorePrinciples.md, п.5: "Формирование природных ресурсов").
// Выполняется один раз при инициализации, до начала тиков — как и
// scatterBoulders, это не System.
//
// Метод (подробности — в TerrainGenerator.cpp):
//   1. Heightmap: фрактальный шум (fBm, OpenSimplex2, несколько октав).
//   2. Priority-Flood (Barnes et al., 2014) — заполнение впадин, отсюда
//      пруды.
//   3. D8 flow accumulation по заполненному рельефу — отсюда реки; часть
//      граничных клеток получает случайный бонус к накоплению, так река
//      может "начинаться" за краем карты.
//   4. Каменистость/утрамбованность подмешиваются в heightmap как бугры —
//      вода их естественно огибает, поэтому там река сама не возникает.
//   5. Влажность — фоновый шум + затухание по расстоянию до воды
//      (multi-source BFS, distance transform).
//
// Все числовые пороги и коэффициенты — в params (TerrainParams.hpp), ни
// одного зашитого значения внутри .cpp.
//
// Создаёт один терраформирующий Entity на тайл: PositionComponent +
// SoilComponent, и WaterComponent — там, где есть река или пруд.
void generateTerrain(World& world, unsigned seed, const TerrainParams& params = TerrainParams{});

} // namespace goblins

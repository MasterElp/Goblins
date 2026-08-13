#pragma once

#include "core/World.hpp"
#include "core/generation/TerrainParams.hpp"

namespace goblins {

// Диагностика одного вызова generateTerrain — не влияет на сам мир,
// только на то, что сервер печатает в консоль. Позволяет отличить
// "генерация просто медленная" от "зависла" (по последней напечатанной
// стадии) и "рек мало, потому что карта тесная" от "рек мало из-за бага"
// (по riverAttemptsUsed/riverAttemptsMax и riverTimedOut).
struct GenerationStats {
    int riversRequested = 0;
    int riversPlaced = 0;
    int riverAttemptsUsed = 0;
    int riverAttemptsMax = 0;
    // true, если стадия рек прервана по таймауту (kRiverStageDeadlineMs в
    // TerrainGenerator.cpp), не исчерпав лимит попыток — верный признак,
    // что путь/штамповка русла где-то ушли в патологически дорогой случай
    // (см. kMaxLateralPerStep, kMaxRiverPathSamples).
    bool riverTimedOut = false;
    // Сколько раз потолок kMaxRiverPathSamples реально обрубал путь —
    // при штатных параметрах должно быть 0 (см. kMaxLateralPerStep).
    int riverPathsCapped = 0;
    int pondComponentsPlaced = 0;

    double heightmapMs = 0.0;
    double riverMs = 0.0;
    double floodFillMs = 0.0;
    double pondMs = 0.0;
    double moistureMs = 0.0;
    double entityMs = 0.0;
    double totalMs = 0.0;
};

// Шаг процедурной генерации мира — формирование почвы и водоёмов
// (02_CorePrinciples.md, п.5: "Формирование природных ресурсов").
// Выполняется один раз при инициализации, до начала тиков — как и
// scatterBoulders, это не System.
//
// Метод (подробности — в TerrainGenerator.cpp):
//   1. Heightmap: фрактальный шум (fBm, OpenSimplex2, несколько октав).
//   2. Реки: явные русла — у каждой реки один случайный исток и один
//      случайный конец на случайных (разных) сторонах карты; путь между
//      ними идёт курсом на цель с боковым отклонением от шума ("меандр")
//      и лёгким притяжением к более низкому рельефу ("немного учитывает
//      почву"). У каждой реки своя случайная скорость потока (до
//      riverMaxFlowSpeed) — чем быстрее река, тем сильнее подавляется
//      боковое отклонение, то есть тем она прямее. Ширина и глубина реки
//      сами дышат вдоль пути (независимый шум). Если путь новой реки
//      утыкается в уже принятую — либо сливается с ней (обрывается в
//      этой точке, целевая река становится шире ниже по течению), либо
//      путь целиком отклоняется и пробуется заново — так русла разных
//      рек не наезжают друг на друга нигде, кроме точек слияния. Русло
//      вырезается в heightmap (понижает рельеф по ширине реки) ДО
//      заливки впадин — поэтому реки естественно взаимодействуют с
//      прудами (могут их пересекать), без отдельной логики совмещения.
//   3. Priority-Flood (Barnes et al., 2014) — заполнение впадин
//      (вырезанного руслами) рельефа, отсюда пруды.
//   4. Каменистость/утрамбованность подмешиваются в heightmap как бугры —
//      вода их естественно огибает, поэтому там ни пруд, ни река сами не
//      возникают без явного русла.
//   5. Влажность — фоновый шум + затухание по расстоянию до воды
//      (multi-source BFS, distance transform).
//   6. Минералы (SoilComponent.minerals, целое число) — шум, в среднем
//      дающий mineralsAverage; на клетках реки не шум, а всегда одно и то
//      же фиксированное riverMinerals. Дальнейшее движение минералов
//      между тайлами по правилу песочной кучи — не здесь, а в
//      HydrologySystem (каждый тик, а не один раз при генерации).
//   7. Свойства мира (WorldPropertiesComponent на World Entity,
//      06_GameLoop.md, п.1a) — выбираются здесь один раз (например,
//      mineralMoistureThreshold) и дальше не меняются System-ами.
//
// Все числовые пороги и коэффициенты — в params (TerrainParams.hpp), ни
// одного зашитого значения внутри .cpp.
//
// Создаёт один терраформирующий Entity на тайл: PositionComponent +
// SoilComponent + HeightComponent, и WaterComponent — там, где есть река или
// пруд.
//
// Возвращает статистику вызова (GenerationStats) — вызывающая сторона
// (server) печатает её в консоль как единственный источник диагностики:
// core сам не делает I/O (07_TechStack.md, п.6: core не знает о
// консоли/JSON, только сервер).
GenerationStats generateTerrain(World& world, unsigned seed, const TerrainParams& params = TerrainParams{});

} // namespace goblins

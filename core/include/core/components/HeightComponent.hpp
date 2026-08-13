#pragma once

namespace goblins {

// Высота рельефа тайла — базовое свойство, как SoilComponent: есть у каждого
// террейн-Entity безусловно, а не только там, где вода. Персистентный аналог
// временного массива elevation из TerrainGenerator.cpp — нужен, чтобы у воды
// (HydrologySystem) было объективное "куда течь под гору", а не случайное
// блуждание (02_CorePrinciples.md, п.15: система вычисляет следующее
// состояние, а не имитирует желаемый результат).
struct HeightComponent {
    float height = 0.0f;
};

} // namespace goblins

#pragma once

namespace goblins {

// Почва — это Entity, а не свойство тайла (04_WorldModel.md, п.3: тайл —
// пустой контейнер; всё, что "на земле", существует как Entity). На
// каждом тайле стоит ровно один такой Entity — терраформирующий слой,
// сгенерированный один раз при инициализации мира.
//
// moisture/rockiness/compaction нормализованы в [0, 1]:
// - moisture   — влажность (выше рядом с водой);
// - rockiness  — каменистость;
// - compaction — утрамбованность.
//
// minerals — количество минералов, целое неотрицательное число (не
// нормализовано: это счётное количество "крупиц", а не доля — см.
// HydrologySystem, где минералы двигаются между соседними тайлами по
// правилу игры в песочную кучу).
struct SoilComponent {
    float moisture = 0.0f;
    float rockiness = 0.0f;
    float compaction = 0.0f;
    int minerals = 0;
};

} // namespace goblins

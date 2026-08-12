#pragma once

namespace goblins {

// Почва — это Entity, а не свойство тайла (04_WorldModel.md, п.3: тайл —
// пустой контейнер; всё, что "на земле", существует как Entity). На
// каждом тайле стоит ровно один такой Entity — терраформирующий слой,
// сгенерированный один раз при инициализации мира.
//
// Все три параметра нормализованы в [0, 1]:
// - moisture   — влажность (выше рядом с водой);
// - rockiness  — каменистость;
// - compaction — утрамбованность.
struct SoilComponent {
    float moisture = 0.0f;
    float rockiness = 0.0f;
    float compaction = 0.0f;
};

} // namespace goblins

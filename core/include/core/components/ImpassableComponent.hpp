#pragma once

namespace goblins {

// Тег-компонент без данных. Его наличие у Entity означает, что Entity
// занимает тайл полностью — это свойство самого Entity, а не тайла
// (04_WorldModel.md, п.4). Отсутствие компонента означает отсутствие
// этой возможности (02_CorePrinciples.md, п.3).
struct ImpassableComponent {};

} // namespace goblins

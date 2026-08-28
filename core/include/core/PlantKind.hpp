#pragma once

#include <cstdint>
#include <string>

#include <entt/entt.hpp>

#include <algorithm>

#include "core/Scale.hpp"
#include "core/components/BushComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"

namespace goblins {

// Какого рода это растение — трава, куст или дерево.
//
// Родов стало три, и с третьим перестал работать приём "метка есть или
// метки нет": `isTree` отвечал на вопрос целиком, а теперь на него нужны
// два вопроса подряд, и задаются они в полудюжине мест — система, файл
// мира, сетевой слой, снимок клеток. Разъехаться этим ответам нельзя:
// растение, посчитанное травой при сохранении и кустом при загрузке, теряет
// ягоды и меняет таблицу черт.
//
// Поэтому род читается ОДНОЙ функцией, а имя его пишется ОДНОЙ парой
// (см. sexName / desireName — тот же приём и та же причина): имя значения
// живёт рядом с самим значением, чтобы файл мира, протокол и вывод сервера
// называли род одинаково.
//
// Сами метки при этом остаются пустыми компонентами (BushComponent,
// TreeComponent): поведение определяется компонентами (02_CorePrinciples.md,
// п.3), и перечисление их не заменяет — оно лишь способ спросить.
// Метки "это трава" нет и не нужно: трава — растение по умолчанию.
enum class PlantKind : std::uint8_t {
    Grass = 0,
    Bush = 1,
    Tree = 2,
};

inline const char* plantKindName(PlantKind kind) {
    switch (kind) {
        case PlantKind::Bush: return "bush";
        case PlantKind::Tree: return "tree";
        case PlantKind::Grass: break;
    }
    return "grass";
}

// Обратная сторона той же пары. Неизвестное имя — трава: род растения в
// старом файле мира может быть не записан вовсе, и это не повод не открыть
// мир.
inline PlantKind plantKindFromName(const std::string& name) {
    if (name == "bush") return PlantKind::Bush;
    if (name == "tree") return PlantKind::Tree;
    return PlantKind::Grass;
}

// Род растения (или семени: метки лежат и на семенах, чтобы семя проросло
// тем же, чем был родитель).
template <typename Registry>
PlantKind plantKindOf(const Registry& registry, entt::entity entity) {
    if (registry.template all_of<TreeComponent>(entity)) {
        return PlantKind::Tree;
    }
    if (registry.template all_of<BushComponent>(entity)) {
        return PlantKind::Bush;
    }
    return PlantKind::Grass;
}

// Долголетие рода — который из трёх множителей мира к нему относится.
//
// Живёт здесь, рядом с самим родом, по той же причине, по какой здесь живёт
// имя: спрашивают это в нескольких местах (система, посев травы, посев
// деревьев, посев кустов), и три копии выбора "какой из трёх" разъехались
// бы молча — дерево начало бы стареть по травяному сроку.
inline int plantLifespanOf(const WorldPropertiesComponent& properties, PlantKind kind) {
    switch (kind) {
        case PlantKind::Bush: return properties.bushLifespan;
        case PlantKind::Tree: return properties.treeLifespan;
        case PlantKind::Grass: break;
    }
    return properties.grassLifespan;
}

// Сроки жизни растения с поправкой на долголетие его рода. Все три —
// предельный возраст, созревание и покой семени: это времена жизни вида, и
// растягивать их порознь значило бы поехать долей взрослой жизни.
//
// Множитель приходит от мира, а не из генома: умножь сам ген — и вложение
// черты посчитается против неумноженных границ таблицы (advantageOf,
// core/generation/Genetics.hpp), обрежется до единицы, и весь бюджет
// преимуществ поедет молча.
inline int plantMaxAgeOf(const PlantGenomeComponent& genome, int lifespan) {
    return std::max(1, static_cast<int>(static_cast<std::int64_t>(genome.maxAge) * lifespan / kFull));
}

inline int plantMaturityAgeOf(const PlantGenomeComponent& genome, int lifespan) {
    return std::max(1, static_cast<int>(static_cast<std::int64_t>(genome.maturityAge) * lifespan / kFull));
}

inline int plantSeedDormancyOf(const PlantGenomeComponent& genome, int lifespan) {
    return std::max(1, static_cast<int>(static_cast<std::int64_t>(genome.seedDormancy) * lifespan / kFull));
}

} // namespace goblins

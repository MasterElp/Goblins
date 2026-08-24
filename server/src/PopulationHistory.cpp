#include "server/PopulationHistory.hpp"

#include <utility>

#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/GoblinTribesComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/generation/GoblinGenetics.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/PlantGenetics.hpp"

namespace goblins {

namespace {

// Длина вектора — по числу видов этого мира, а не по максимальному
// встреченному индексу: вымерший до последней особи вид обязан остаться в
// летописи нулём. Иначе его кривая просто обрывалась бы, и вымирание было
// бы не отличить от "такого вида в мире и не было".
std::vector<int> countPlants(const World& world) {
    const auto& registry = world.registry();
    const auto& archetypes = registry.get<const PlantSpeciesComponent>(world.worldEntity()).grasses;
    std::vector<int> counts(archetypes.size(), 0);
    // Именно живые растения: геном есть и у лежащего в клетке семени
    // (SeedComponent), но семя — ещё не растение, и считать его в
    // численности вида значило бы рисовать на графике то, чего на лугу
    // не видно.
    //
    // И именно трава: у дерева свой список видов и своя нумерация
    // (PlantSpeciesComponent), поэтому сложить их в один вектор значило бы
    // смешать два разных нумерования — ровно та же причина, по которой
    // травоядные и хищники считаются порознь.
    registry.view<const PlantComponent, const PlantGenomeComponent>().each(
        [&](const entt::entity entity, const PlantComponent& /*plant*/, const PlantGenomeComponent& genome) {
            if (registry.all_of<TreeComponent>(entity)) {
                return;
            }
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < counts.size()) {
                ++counts[static_cast<std::size_t>(genome.species)];
            }
        });
    return counts;
}

// Деревья — своим вектором, а не вместе с травой: нумерация видов у них
// своя (PlantSpeciesComponent), и вид 0 на общем графике оказался бы то
// травой, то рощей. Та же причина, по которой порознь считаются травоядные
// и хищники.
std::vector<int> countTrees(const World& world) {
    const auto& registry = world.registry();
    const auto& archetypes = registry.get<const PlantSpeciesComponent>(world.worldEntity()).trees;
    std::vector<int> counts(archetypes.size(), 0);
    registry.view<const TreeComponent, const PlantComponent, const PlantGenomeComponent>().each(
        [&](const PlantComponent& /*plant*/, const PlantGenomeComponent& genome) {
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < counts.size()) {
                ++counts[static_cast<std::size_t>(genome.species)];
            }
        });
    return counts;
}

// Животные — по одному вектору на диету: индекс вида у травоядных и у
// хищников свой, и сложить их в один вектор значило бы смешать два разных
// нумерования. wantPredators выбирает, чьё поголовье считается.
std::vector<int> countAnimals(const World& world, bool wantPredators) {
    const auto& registry = world.registry();
    const auto& species = registry.get<const AnimalSpeciesComponent>(world.worldEntity());
    const auto& archetypes = wantPredators ? species.predators : species.herbivores;
    std::vector<int> counts(archetypes.size(), 0);
    // Гоблин носит то же тело и тот же тип генома (core/Body.hpp), поэтому
    // перебор "по телу" его захватывает — а species у него означает племя, и
    // без исключения он приписывался бы к виду травоядных с тем же номером.
    registry.view<const AnimalComponent, const AnimalGenomeComponent>(entt::exclude<GoblinComponent>)
        .each(
        [&](const entt::entity entity, const AnimalComponent& /*animal*/, const AnimalGenomeComponent& genome) {
            if (registry.all_of<PredatorComponent>(entity) != wantPredators) {
                return;
            }
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < counts.size()) {
                ++counts[static_cast<std::size_t>(genome.species)];
            }
        });
    return counts;
}

// Средний геном живых: по одному числу на черту таблицы. Среднее
// считается по всем особям диеты сразу — вопрос, ради которого оно
// записывается, звучит "куда сносит мир целиком".
//
// Копится в int64: гены целые, а их сумма по тысячам растений (у max_age
// это тысячи на особь) вышла бы за int уже на среднем лугу. Делится
// нацело — округление до единицы гена не важно там, где интересна форма
// кривой за тысячи тиков, а дробное среднее пришлось бы куда-то ронять,
// и в летописи появилась бы дробь (core/Scale.hpp: их в состоянии мира
// нет).
//
// Живых ноль — вектор пустой, а не нулевой: "средний геном вымерших" не
// значит ничего, и рисовать по нему ноль было бы враньём. Клиент такую
// точку просто не начинает (тем же способом, каким переживает точки из
// мира без хищников).
template <typename Traits, typename Each>
std::vector<int> averageGenome(const Traits& traits, std::size_t traitCount, Each each) {
    std::vector<std::int64_t> sums(traitCount, 0);
    std::int64_t count = 0;
    each([&](const auto& genome) {
        ++count;
        std::size_t i = 0;
        for (const auto& trait : traits) {
            sums[i++] += genome.*trait.gene;
        }
    });
    if (count == 0) {
        return {};
    }
    std::vector<int> average(traitCount, 0);
    for (std::size_t i = 0; i < traitCount; ++i) {
        average[i] = static_cast<int>(sums[i] / count);
    }
    return average;
}

std::vector<int> averagePlantGenome(const World& world) {
    const auto& registry = world.registry();
    // Только живые растения — по той же причине, по которой их считает
    // countPlants: у лежащего семени геном есть, но на лугу его нет.
    return averageGenome(kGrassTraits, kGrassTraitCount, [&](auto&& visit) {
        registry.view<const PlantComponent, const PlantGenomeComponent>().each(
            [&](const PlantComponent& /*plant*/, const PlantGenomeComponent& genome) { visit(genome); });
    });
}

std::vector<int> averageTreeGenome(const World& world) {
    const auto& registry = world.registry();
    return averageGenome(kTreeTraits, kTreeTraitCount, [&](auto&& visit) {
        registry.view<const TreeComponent, const PlantComponent, const PlantGenomeComponent>().each(
            [&](const PlantComponent& /*plant*/, const PlantGenomeComponent& genome) { visit(genome); });
    });
}

// Гоблины — по племенам. Своя функция, а не третий случай в countAnimals:
// племена живут в своём компоненте и своей таблице черт, и общий параметр
// "кого считать" пришлось бы делать перечислением из трёх значений ради
// одной ветки.
std::vector<int> countGoblins(const World& world) {
    const auto& registry = world.registry();
    const auto& tribes = registry.get<const GoblinTribesComponent>(world.worldEntity());
    std::vector<int> counts(tribes.tribes.size(), 0);
    registry.view<const AnimalComponent, const AnimalGenomeComponent, const GoblinComponent>().each(
        // GoblinComponent в списке параметров нет: пустой тег EnTT не
        // хранит и в each не передаёт.
        [&](const AnimalComponent& /*body*/, const AnimalGenomeComponent& genome) {
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < counts.size()) {
                ++counts[static_cast<std::size_t>(genome.species)];
            }
        });
    return counts;
}

std::vector<int> averageGoblinGenome(const World& world) {
    const auto& registry = world.registry();
    const auto traits = goblinTraits();
    return averageGenome(traits, traits.size(), [&](auto&& visit) {
        registry.view<const AnimalComponent, const AnimalGenomeComponent, const GoblinComponent>().each(
            [&](const AnimalComponent& /*body*/, const AnimalGenomeComponent& genome) { visit(genome); });
    });
}

std::vector<int> averageAnimalGenome(const World& world, bool wantPredators) {
    const auto& registry = world.registry();
    const auto traits = wantPredators ? predatorTraits() : herbivoreTraits();
    return averageGenome(traits, traits.size(), [&](auto&& visit) {
        registry.view<const AnimalComponent, const AnimalGenomeComponent>(entt::exclude<GoblinComponent>)
            .each([&](const entt::entity entity, const AnimalComponent& /*animal*/,
                      const AnimalGenomeComponent& genome) {
                if (registry.all_of<PredatorComponent>(entity) == wantPredators) {
                    visit(genome);
                }
            });
    });
}

// Точка — массивом: тик, численность травы по видам, травоядных по видам,
// хищников по видам. Одно место на файл сохранения и на протокол:
// разъехавшись, они читались бы одним и тем же клиентом по-разному.
//
// Хищники — четвёртым элементом, дописанным к трём прежним, а не новым
// полем в объекте; средние геномы — тремя следующими, тем же способом.
// Летописи миров, прожитых до их появления, короче на эти элементы, и
// читающая сторона обязана это пережить (см. fromJson).
//
// Элемент, дописанный в конец, — единственный способ растить эту запись
// без смены формата: имена полей у тысячи точек весили бы больше самих
// чисел, а порядковый номер ничего не весит вовсе.
nlohmann::json encodePoint(const PopulationHistory::Point& point) {
    auto entry = nlohmann::json::array();
    entry.push_back(point.tick);
    entry.push_back(point.plants);
    entry.push_back(point.herbivores);
    entry.push_back(point.predators);
    entry.push_back(point.plantGenome);
    entry.push_back(point.herbivoreGenome);
    entry.push_back(point.predatorGenome);
    // Деревья дописаны последними, как в своё время хищники и средние
    // геномы: элемент в конец — единственный способ растить эту запись без
    // смены формата. Летописи миров, прожитых до деревьев, короче на эти
    // два элемента, и читающая сторона обязана это пережить (см. fromJson).
    entry.push_back(point.trees);
    entry.push_back(point.treeGenome);
    // Гоблины — тем же способом и по той же причине: элемент в конец.
    // Летописи миров, прожитых до них, короче на эти два элемента.
    entry.push_back(point.goblins);
    entry.push_back(point.goblinGenome);
    return entry;
}

// Массив целых из JSON, пришедшего снаружи (файл сохранения могли поправить
// руками, он мог обрезаться при падении). Нечисло — не повод потерять всю
// летопись, поэтому читается то, что читается.
std::vector<int> readInts(const nlohmann::json& json) {
    std::vector<int> result;
    if (!json.is_array()) {
        return result;
    }
    result.reserve(json.size());
    for (const auto& value : json) {
        result.push_back(value.is_number() ? value.get<int>() : 0);
    }
    return result;
}

} // namespace

void PopulationHistory::clear() {
    points_.clear();
    interval_ = kBaseIntervalTicks;
}

void PopulationHistory::record(const World& world) {
    const auto tick = world.registry().get<const TimeComponent>(world.worldEntity()).tick;

    // Первая точка пишется всегда — это состояние мира сразу после
    // генерации (или на момент загрузки), и без неё кривая начиналась бы
    // с середины.
    if (!points_.empty() && tick < points_.back().tick + interval_) {
        return;
    }

    Point point;
    point.tick = tick;
    point.plants = countPlants(world);
    point.trees = countTrees(world);
    point.herbivores = countAnimals(world, /*wantPredators=*/false);
    point.predators = countAnimals(world, /*wantPredators=*/true);
    point.plantGenome = averagePlantGenome(world);
    point.treeGenome = averageTreeGenome(world);
    point.herbivoreGenome = averageAnimalGenome(world, /*wantPredators=*/false);
    point.predatorGenome = averageAnimalGenome(world, /*wantPredators=*/true);
    point.goblins = countGoblins(world);
    point.goblinGenome = averageGoblinGenome(world);
    points_.push_back(std::move(point));

    if (points_.size() > kMaxPoints) {
        thin();
    }
}

void PopulationHistory::thin() {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < points_.size(); i += 2) {
        if (kept != i) {
            points_[kept] = std::move(points_[i]);
        }
        ++kept;
    }
    points_.resize(kept);
    interval_ *= 2;
}

nlohmann::json PopulationHistory::toJson() const {
    nlohmann::json json;
    json["interval"] = interval_;
    auto points = nlohmann::json::array();
    for (const auto& point : points_) {
        points.push_back(encodePoint(point));
    }
    json["points"] = std::move(points);
    return json;
}

nlohmann::json PopulationHistory::toJson(std::uint64_t afterTick) const {
    nlohmann::json json;
    json["interval"] = interval_;
    auto points = nlohmann::json::array();
    for (const auto& point : points_) {
        if (point.tick > afterTick) {
            points.push_back(encodePoint(point));
        }
    }
    json["points"] = std::move(points);
    return json;
}

void PopulationHistory::fromJson(const nlohmann::json& json) {
    clear();
    if (!json.is_object()) {
        return;
    }

    const auto interval = json.value("interval", kBaseIntervalTicks);
    interval_ = interval > 0 ? interval : kBaseIntervalTicks;

    if (!json.contains("points") || !json["points"].is_array()) {
        return;
    }

    for (const auto& entry : json["points"]) {
        if (!entry.is_array() || entry.size() < 3 || !entry[0].is_number_unsigned()) {
            continue;
        }
        Point point;
        point.tick = entry[0].get<std::uint64_t>();
        // Время в летописи может только идти вперёд: точка не позже
        // предыдущей — признак битого файла, и на графике она дала бы
        // скачок назад по оси.
        if (!points_.empty() && point.tick <= points_.back().tick) {
            continue;
        }
        point.plants = readInts(entry[1]);
        point.herbivores = readInts(entry[2]);
        // Точка из мира, прожитого до появления хищников, короче на
        // элемент. Это не битый файл, а другое прошлое: хищников в тот тик
        // не было вовсе, и пустой вектор говорит именно это.
        if (entry.size() > 3) {
            point.predators = readInts(entry[3]);
        }
        // Средние геномы дописаны позже хищников: у точек постарше их нет,
        // и это не битый файл, а другое прошлое. Пустой вектор говорит
        // "тогда этого не записывали" — на графике такая часть кривой
        // просто не начата.
        if (entry.size() > 6) {
            point.plantGenome = readInts(entry[4]);
            point.herbivoreGenome = readInts(entry[5]);
            point.predatorGenome = readInts(entry[6]);
        }
        // Деревья дописаны позже средних геномов — тем же способом и с той
        // же оговоркой: у точек постарше их нет, и это не битый файл, а
        // другое прошлое, в котором деревьев не было вовсе.
        if (entry.size() > 8) {
            point.trees = readInts(entry[7]);
            point.treeGenome = readInts(entry[8]);
        }
        points_.push_back(std::move(point));
    }

    // Файл мог быть записан сервером с другим потолком точек.
    while (points_.size() > kMaxPoints) {
        thin();
    }
}

} // namespace goblins

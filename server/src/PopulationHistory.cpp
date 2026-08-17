#include "server/PopulationHistory.hpp"

#include <utility>

#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/TimeComponent.hpp"

namespace goblins {

namespace {

// Длина вектора — по числу видов этого мира, а не по максимальному
// встреченному индексу: вымерший до последней особи вид обязан остаться в
// летописи нулём. Иначе его кривая просто обрывалась бы, и вымирание было
// бы не отличить от "такого вида в мире и не было".
std::vector<int> countPlants(const World& world) {
    const auto& registry = world.registry();
    const auto& archetypes = registry.get<const PlantSpeciesComponent>(world.worldEntity()).archetypes;
    std::vector<int> counts(archetypes.size(), 0);
    // Именно живые растения: геном есть и у лежащего в клетке семени
    // (SeedComponent), но семя — ещё не растение, и считать его в
    // численности вида значило бы рисовать на графике то, чего на лугу
    // не видно.
    registry.view<const PlantComponent, const PlantGenomeComponent>().each(
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
    registry.view<const AnimalComponent, const AnimalGenomeComponent>().each(
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

// Точка — массивом: тик, численность травы по видам, травоядных по видам,
// хищников по видам. Одно место на файл сохранения и на протокол:
// разъехавшись, они читались бы одним и тем же клиентом по-разному.
//
// Хищники — четвёртым элементом, дописанным к трём прежним, а не новым
// полем в объекте: летописи миров, прожитых до их появления, короче на
// элемент, и читающая сторона обязана это пережить (см. fromJson).
nlohmann::json encodePoint(const PopulationHistory::Point& point) {
    auto entry = nlohmann::json::array();
    entry.push_back(point.tick);
    entry.push_back(point.plants);
    entry.push_back(point.herbivores);
    entry.push_back(point.predators);
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
    point.herbivores = countAnimals(world, /*wantPredators=*/false);
    point.predators = countAnimals(world, /*wantPredators=*/true);
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
        points_.push_back(std::move(point));
    }

    // Файл мог быть записан сервером с другим потолком точек.
    while (points_.size() > kMaxPoints) {
        thin();
    }
}

} // namespace goblins

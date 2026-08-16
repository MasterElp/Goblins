#include "server/PopulationHistory.hpp"

#include <utility>

#include "core/components/HerbivoreComponent.hpp"
#include "core/components/HerbivoreGenomeComponent.hpp"
#include "core/components/HerbivoreSpeciesComponent.hpp"
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
    registry.view<const PlantGenomeComponent>().each([&](const PlantGenomeComponent& genome) {
        if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < counts.size()) {
            ++counts[static_cast<std::size_t>(genome.species)];
        }
    });
    return counts;
}

std::vector<int> countHerbivores(const World& world) {
    const auto& registry = world.registry();
    const auto& archetypes = registry.get<const HerbivoreSpeciesComponent>(world.worldEntity()).archetypes;
    std::vector<int> counts(archetypes.size(), 0);
    registry.view<const HerbivoreComponent, const HerbivoreGenomeComponent>().each(
        [&](const HerbivoreComponent& /*animal*/, const HerbivoreGenomeComponent& genome) {
            if (genome.species >= 0 && static_cast<std::size_t>(genome.species) < counts.size()) {
                ++counts[static_cast<std::size_t>(genome.species)];
            }
        });
    return counts;
}

// Точка — массивом из трёх элементов: тик, численность травы по видам,
// численность травоядных по видам. Одно место на файл сохранения и на
// протокол: разъехавшись, они читались бы одним и тем же клиентом
// по-разному.
nlohmann::json encodePoint(const PopulationHistory::Point& point) {
    auto entry = nlohmann::json::array();
    entry.push_back(point.tick);
    entry.push_back(point.plants);
    entry.push_back(point.herbivores);
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
    point.herbivores = countHerbivores(world);
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
        points_.push_back(std::move(point));
    }

    // Файл мог быть записан сервером с другим потолком точек.
    while (points_.size() > kMaxPoints) {
        thin();
    }
}

} // namespace goblins

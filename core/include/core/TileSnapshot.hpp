#pragma once

#include <cstddef>
#include <vector>

#include <entt/entt.hpp>

#include "core/World.hpp"
#include "core/PlantKind.hpp"
#include "core/components/BerryComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/StoreComponent.hpp"
#include "core/Build.hpp"
#include "core/components/BuildingComponent.hpp"
#include "core/components/SiteComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/components/WaterComponent.hpp"

namespace goblins {

// Что лежит на клетках прямо сейчас — плотными массивами, по значению на
// клетку.
//
// Плотные массивы здесь уместны в отличие от животных: почвы и травы по
// одной на клетку, а животных десятки на десятки тысяч клеток (тот же
// приём, что в HydrologySystem и PlantSystem).
//
// Смысл снимка не в скорости, а в законе: он снимается ДО того, как хоть
// кто-нибудь тронулся с места, поэтому все решения тика принимаются по
// одному и тому же состоянию мира, и порядок обхода Entity на них не влияет
// (04_WorldModel.md, п.8).
//
// Снимает его КАЖДАЯ система себе и в начале своего прохода, а не один на
// весь тик: системы идут по очереди, и та, что идёт следом, обязана видеть
// траву такой, какой её оставила предыдущая (05_Entity.md, п.6 — системы
// разговаривают только состоянием компонентов). Общий на весь тик снимок
// показывал бы гоблину траву, которую стадо уже съело.
//
// Объект живёт у вызывающей стороны, внутри одного вызова системы, и
// переиспользуется между её внутренними проходами. Между тиками он не
// живёт: система не хранит состояние (05_Entity.md, п.3).
struct TileSnapshot {
    int width = 0;
    int height = 0;

    // Терраформирующий Entity тайла: почва, высота, вода, перегной, падаль.
    // entt::null означает "клетки нет" — за краем Области или там, где земли
    // не создали.
    std::vector<entt::entity> terrain;
    // Глубина воды; 0 — воды нет. Для ноги это стена (см. standableAt в
    // core/Path.hpp).
    std::vector<int> waterAt;
    // ТРАВА на клетке и её развитость — она же съедобная биомасса.
    //
    // Только трава, и это не мелочь: раньше здесь лежало любое растение, и
    // травоядное щипало заодно крону дерева — то самое, про которое ниже
    // написано "не еда". Комментарий говорил одно, код делал другое; теперь
    // деревья и кусты лежат своими массивами, и объесть их отсюда нельзя
    // никак.
    std::vector<entt::entity> plantAt;
    std::vector<int> plantGrowth;
    // КУСТ на клетке и сколько на нём висит ягод.
    //
    // Куст не пасут: с него рвут ягоды, а сам он остаётся стоять
    // (core/Berries.hpp). Поэтому у него не развитость, а счёт ягод — только
    // они и съедобны, и только их и ищут глазами.
    std::vector<entt::entity> bushAt;
    std::vector<int> berriesAt;
    // Мясо лежащей туши.
    std::vector<int> carcassMeat;
    // Что лежит в куче на клетке (core/Store.hpp): еда — рядом с падалью не
    // случайно, и то, и другое съедобное состояние земли, — а материал нужен
    // стройке. Порознь по видам, потому что и берут их порознь: еду в рот,
    // солому с ветками в дело.
    std::vector<int> storeFood;
    std::vector<int> storeMaterial;
    // Сколько всего лежит на клетке: по нему видно, влезет ли ещё
    // (kStoreCapacity, core/Store.hpp).
    std::vector<int> storeTotal;
    // Влажность и каменистость почвы. Ходьбе они не мешают ничем, а вот
    // лежанию мешают обе (core/Rest.hpp): мокро и жёстко.
    std::vector<int> moisture;
    std::vector<int> rockiness;
    // Утоптанность: насколько землю умяли ногами (core/Trample.hpp). Ходьбе
    // она, в отличие от тех двух, как раз помогает — по натоптанному идти
    // легче (седьмое слагаемое шага, core/Walk.hpp).
    std::vector<int> trampled;
    // Дерево — не еда (объедать крону травоядное не умеет), а укрытие: под
    // ним добычу не высматривают (kCoverSight, core/Hunting.hpp), и к нему
    // же бежит испуганный. С появлением стройки оно стало ещё и источником
    // веток, поэтому рядом с меткой лежит и само растение с его развитостью:
    // ветку ломают у дерева, а не у флага.
    std::vector<unsigned char> treeAt;
    std::vector<entt::entity> treeEntity;
    std::vector<int> treeGrowth;

    // Что на клетке построено и что на ней строится (core/Build.hpp).
    // Прочность — 0..kFull, ноль значит "нет"; вид площадки — BuildKind::None,
    // если стройки нет.
    std::vector<int> canopy;
    std::vector<int> bedding;
    std::vector<BuildKind> siteKind;

    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    }

    std::size_t cellCount() const {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    void capture(const World& world) {
        width = world.area().width();
        height = world.area().height();
        const std::size_t cells = cellCount();

        // assign, а не resize: массивы переиспользуются, и остатки прошлого
        // снимка обязаны быть стёрты. Клетка, с которой ушла вода, иначе
        // осталась бы для ноги стеной.
        terrain.assign(cells, entt::null);
        waterAt.assign(cells, 0);
        plantAt.assign(cells, entt::null);
        plantGrowth.assign(cells, 0);
        bushAt.assign(cells, entt::null);
        berriesAt.assign(cells, 0);
        carcassMeat.assign(cells, 0);
        storeFood.assign(cells, 0);
        storeMaterial.assign(cells, 0);
        storeTotal.assign(cells, 0);
        moisture.assign(cells, 0);
        rockiness.assign(cells, 0);
        trampled.assign(cells, 0);
        treeAt.assign(cells, 0);
        treeEntity.assign(cells, entt::null);
        treeGrowth.assign(cells, 0);
        canopy.assign(cells, 0);
        bedding.assign(cells, 0);
        siteKind.assign(cells, BuildKind::None);
        if (cells == 0) {
            return;
        }

        const auto& registry = world.registry();

        auto terrainView = registry.view<const PositionComponent, const SoilComponent>();
        for (const auto entity : terrainView) {
            const auto& position = terrainView.get<const PositionComponent>(entity);
            if (!world.area().inBounds(position.x, position.y)) {
                continue;
            }
            const std::size_t i = index(position.x, position.y);
            terrain[i] = entity;
            const auto& soil = terrainView.get<const SoilComponent>(entity);
            moisture[i] = soil.moisture;
            rockiness[i] = soil.rockiness;
            trampled[i] = soil.trampled;
            if (const auto* water = registry.try_get<const WaterComponent>(entity)) {
                waterAt[i] = water->depth;
            }
            if (const auto* carcass = registry.try_get<const CarcassComponent>(entity)) {
                carcassMeat[i] = carcass->meat;
            }
            if (const auto* store = registry.try_get<const StoreComponent>(entity)) {
                storeFood[i] = store->stored.of(ResourceKind::Food);
                storeMaterial[i] = materialIn(store->stored);
                storeTotal[i] = store->stored.total();
            }
            if (const auto* building = registry.try_get<const BuildingComponent>(entity)) {
                canopy[i] = building->canopy;
                bedding[i] = building->bedding;
            }
            if (const auto* site = registry.try_get<const SiteComponent>(entity)) {
                siteKind[i] = site->kind;
            }
        }

        auto plantView = registry.view<const PlantComponent, const PositionComponent>();
        for (const auto entity : plantView) {
            const auto& position = plantView.get<const PositionComponent>(entity);
            if (!world.area().inBounds(position.x, position.y)) {
                continue;
            }
            const std::size_t i = index(position.x, position.y);
            // Род растения — одной функцией на весь проект (core/PlantKind.hpp):
            // спрашивают его здесь, в файле мира и в сетевом слое, и разъехаться
            // этим ответам нельзя.
            switch (plantKindOf(registry, entity)) {
                case PlantKind::Grass:
                    plantAt[i] = entity;
                    plantGrowth[i] = plantView.get<const PlantComponent>(entity).growth;
                    break;
                case PlantKind::Bush:
                    bushAt[i] = entity;
                    if (const auto* berries = registry.try_get<const BerryComponent>(entity)) {
                        berriesAt[i] = berries->berries;
                    }
                    break;
                case PlantKind::Tree:
                    treeAt[i] = 1;
                    treeEntity[i] = entity;
                    treeGrowth[i] = plantView.get<const PlantComponent>(entity).growth;
                    break;
            }
        }
    }
};

} // namespace goblins

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/Rest.hpp"
#include "core/Scale.hpp"
#include "core/Work.hpp"
#include "core/World.hpp"
#include "core/components/BuildingComponent.hpp"
#include "core/components/SiteComponent.hpp"
#include "core/components/SoilComponent.hpp"

namespace goblins {

// Как строят и из чего — ОДИН закон на всех, кто спрашивает: гоблин (строить),
// PlantSystem и HydrologySystem (ветшать), наблюдатель (рисовать). Про существ
// не знает ничего и принимает числа.
//
// Постройка — первое, чем гоблин МЕНЯЕТ место, а не пользуется им. До сих пор
// он выбирал: где лечь, куда вернуться, где сложить запас. Теперь он делает
// место лучше, чем оно было, — и платит за это трудом, материалом и тем, что
// сделанное надо поддерживать.

// Раз во сколько тиков постройка теряет единицу прочности. Полное обветшание
// — четыре тысячи тиков, как полное зарастание тропы (kTrampleRecoverPeriod,
// core/Trample.hpp), и это соседство не случайно: заброшенный лагерь обязан
// исчезать с той же скоростью, с какой исчезает дорога к нему.
//
// Ветшание — от времени и только от времени. Не от того, кто на постройке
// спал: наказание за людность превратило бы большой лагерь в самый непрочный,
// а он должен быть самым крепким — просто потому, что в нём больше рук.
constexpr int kBuildDecayPeriod = 4;

// Сколько материала съедает одна единица труда. Навес (kWorkCanopy = 400
// работ) требует поэтому 2000 материала — ровно полную горсть взрослого
// (kCarryPerSize, core/Carry.hpp). Один поход за ветками — одна постройка,
// если ходить за ветками, а не за травой.
constexpr int kMaterialPerWork = 5;

// Во сколько соломин ценится ветка. В этом всё различие материалов: строить
// можно из чего угодно, но за ветками стоит идти — их нужно втрое меньше.
constexpr int kTwigStrength = 3;

// Сколько травы срезается и сколько веток ломается за один тик. Ветка тяжелее
// в добыче и втрое ценнее в деле: за один заход рук наберётся вдвое меньше
// веток, чем травы, но стоить они будут вшестеро больше.
constexpr int kStrawHarvest = 200;
constexpr int kTwigHarvest = 100;

// Ниже какой развитости растение не трогают. Не жалость: с ободранного до
// нуля куста нечего взять и в следующий раз, а лагерю жить здесь долго.
// Число то же, с которого растение вообще считается едой (kMinBiteGrowth,
// core/Body.hpp) — граница "тут ещё что-то есть" в мире одна.
constexpr int kHarvestMinGrowth = 100;

// Сила принесённого: солома да ветки, приведённые к одной мерке.
inline int materialStrength(int straw, int twigs) {
    return straw + twigs * kTwigStrength;
}

// Во сколько работ обходится полная прочность этого вида.
inline int buildWorkCost(BuildKind kind) {
    return kind == BuildKind::Canopy ? kWorkCanopy : kWorkBedding;
}

// Прочность этого вида на клетке — и ссылка на неё, чтобы работа и ветшание
// правили одно и то же поле, а не каждый своё.
inline int& buildingCondition(BuildingComponent& building, BuildKind kind) {
    return kind == BuildKind::Canopy ? building.canopy : building.bedding;
}

// Пора ли постройкам на этой клетке обветшать. Сдвиг по номеру клетки — чтобы
// весь лагерь не осыпался в один тик: срок один на всех, очередь у каждого
// своя. Тот же приём, что у ягод и троп.
inline bool buildDecays(std::uint64_t tick, std::size_t cell) {
    return (tick + static_cast<std::uint64_t>(cell)) % static_cast<std::uint64_t>(kBuildDecayPeriod) == 0;
}

// Единица труда на площадке: съесть материал, поднять прочность.
//
// false — работать нечем: материала на площадке не осталось. Это не ошибка, а
// обычное дело — принесут ещё.
inline bool applyWork(SiteComponent& site, BuildingComponent& building) {
    if (site.kind == BuildKind::None) {
        return false;
    }
    int& condition = buildingCondition(building, site.kind);
    if (condition >= kFull) {
        return false;
    }
    if (materialStrength(site.straw, site.twigs) < kMaterialPerWork) {
        return false;
    }

    // Тратится сперва трава: ветки берегутся на то, чего травой не покроешь.
    // Разницы для готовой постройки нет — прочность одна, — но пока на
    // площадке лежит и то, и другое, разумнее класть в дело дешёвое.
    int spend = kMaterialPerWork;
    const int fromStraw = std::min(site.straw, spend);
    site.straw -= fromStraw;
    spend -= fromStraw;
    while (spend > 0 && site.twigs > 0) {
        --site.twigs;
        spend -= kTwigStrength;
    }

    // Единица труда даёт kFull/стоимость прочности — целой она почти никогда
    // не выходит, поэтому остаток ждёт в самой площадке (core/Scale.hpp).
    const int cost = workCost(buildWorkCost(site.kind));
    site.progress += kFull;
    condition = std::min(kFull, condition + site.progress / cost);
    site.progress %= cost;
    return true;
}

// Что этому месту нужнее. Ответ считается не признаками ("сыро — значит
// подстилка"), а тем, СКОЛЬКО КАЖДАЯ ПОСТРОЙКА ЗДЕСЬ ДОБАВИТ: закон отдыха
// уже знает про место всё, и спрашивать его второй раз другими словами было
// бы заведением второго закона.
//
// Отсюда само собой и получается правило "строят то, чего не хватает": на
// открытой поляне навес добавит четыреста, а под деревом — ничего (крыша
// одна, берётся лучшая), и там гоблин положит подстилку.
inline BuildKind betterBuild(const RestPlace& place) {
    const int roof = std::max(place.tree ? kRestShelter : 0,
                              kRestCanopy * std::clamp(place.canopy, 0, kFull) / kFull);
    const int canopyGain = std::max(0, kRestCanopy - roof);
    const int beddingGain = kRestBedding - kRestBedding * std::clamp(place.bedding, 0, kFull) / kFull;
    if (canopyGain <= 0 && beddingGain <= 0) {
        return BuildKind::None;
    }
    return canopyGain >= beddingGain ? BuildKind::Canopy : BuildKind::Bedding;
}

// Положить площадку на клетку. Через очередь команд (05_Entity.md, п.5):
// добавление компонента — структурное изменение. Если площадка там уже есть,
// вторая не ставится: замысел на клетке один.
inline void placeSite(World& world, int x, int y, BuildKind kind) {
    if (kind == BuildKind::None || !world.area().inBounds(x, y)) {
        return;
    }
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!world.registry().all_of<SoilComponent>(tile)) {
            continue;
        }
        if (world.registry().all_of<SiteComponent>(tile)) {
            return;
        }
        world.registry().emplace<SiteComponent>(tile, SiteComponent{kind});
        return;
    }
}

} // namespace goblins

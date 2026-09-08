#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/Rest.hpp"
#include "core/Resources.hpp"
#include "core/Scale.hpp"
#include "core/Work.hpp"
#include "core/World.hpp"
#include "core/components/BuildingComponent.hpp"
#include "core/components/SiteComponent.hpp"
#include "core/components/CarriedComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/StoreComponent.hpp"
#include "core/components/TreeComponent.hpp"

namespace goblins {

// Как строят и из чего — ОДИН закон на всех, кто спрашивает: гоблин (строить),
// PlantSystem и HydrologySystem (ветшать), наблюдатель (рисовать). Про существ
// не знает ничего и принимает числа.
//
// Постройка — первое, чем гоблин МЕНЯЕТ место, а не пользуется им. До сих пор
// он выбирал: где лечь, куда вернуться, где сложить запас. Теперь он делает
// место лучше, чем оно было, — и платит за это трудом, материалом и тем, что
// сделанное надо поддерживать.

// Раз во сколько тиков постройка теряет единицу прочности. Полное обветшание —
// сорок тысяч тиков: вдесятеро дольше, чем зарастает тропа
// (kTrampleRecoverPeriod, core/Trample.hpp), и это верно по сути — тропу
// набивают ногами мимоходом, а навес ставят руками.
//
// Число подобрано замером, а не на глаз. При четырёх тиках поддержание навеса
// стоило около сотни работ на тысячу тиков — четверть всего времени гоблина, —
// и лагерь жил в вечном ремонте: двадцать начатых строек на пятнадцать живых,
// средняя прочность навеса 59 из ста. Теперь поддержание стоит десяти работ на
// тысячу тиков: заметно, но не вместо жизни.
//
// Ветшание — от времени и только от времени. Не от того, кто на постройке
// спал: наказание за людность превратило бы большой лагерь в самый непрочный,
// а он должен быть самым крепким — просто потому, что в нём больше рук.
// Число идёт В НОГУ с темпом жизни (core/Scale.hpp) и умножено под нынешний
// десятикратный. Связь молчаливая, и проверки на неё нет: у этого срока нет
// породы, значит нет и множителя, которым его делить. Меняешь темп — меняй
// и здесь.
//
// Разойдись они — лагерь жил бы в вечном ремонте: работа идёт вдесятеро
// медленнее, а навес ветшал бы с прежней скоростью.
constexpr int kBuildDecayPeriod = 400;

// Сколько материала съедает одна единица труда. Навес (kWorkCanopy = 500
// работ) требует поэтому 2000 материала — ровно полную горсть взрослого
// (kCarryPerSize, core/Carry.hpp). Один поход за ветками — одна постройка,
// если ходить за ветками, а не за травой.
constexpr int kMaterialPerWork = 4;

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
    switch (kind) {
        case BuildKind::Canopy: return kWorkCanopy;
        case BuildKind::Fence: return kWorkFence;
        case BuildKind::Bedding:
        case BuildKind::None: break;
    }
    return kWorkBedding;
}

// Прочность этого вида на клетке — и ссылка на неё, чтобы работа и ветшание
// правили одно и то же поле, а не каждый своё.
// Ответ для BuildKind::None — поле подстилки, и это не выбор, а заглушка:
// работать с видом None нельзя (applyWork проверяет это первой строкой), а
// ссылка обязана быть на что-то.
inline int& buildingCondition(BuildingComponent& building, BuildKind kind) {
    switch (kind) {
        case BuildKind::Canopy: return building.canopy;
        case BuildKind::Fence: return building.fence;
        case BuildKind::Bedding:
        case BuildKind::None: break;
    }
    return building.bedding;
}

inline int buildingConditionOf(const BuildingComponent& building, BuildKind kind) {
    switch (kind) {
        case BuildKind::Canopy: return building.canopy;
        case BuildKind::Fence: return building.fence;
        case BuildKind::Bedding:
        case BuildKind::None: break;
    }
    return building.bedding;
}

// Сколько прочности даёт одна единица труда. Делится нацело — цены построек
// для того и выбраны (core/Work.hpp): дробной прочности в мире нет, и
// хранить неделящийся остаток негде.
inline int conditionPerWork(BuildKind kind) {
    const int cost = workCost(buildWorkCost(kind));
    return cost > 0 ? kFull / cost : 0;
}

// Что здесь ещё не доделано: замысел, если он есть, или начатая, но не
// доведённая до полной прочности постройка. BuildKind::None — доделывать
// нечего.
//
// Одна функция на всех спрашивающих: гоблин (чем заняться), наблюдатель
// (что показать), проверка (стоит ли помнить это место). Разъехаться этим
// ответам нельзя — иначе гоблин будет ходить достраивать то, что для
// наблюдателя достроено.
inline BuildKind unfinishedAt(const BuildingComponent& building, BuildKind site) {
    if (site != BuildKind::None && buildingConditionOf(building, site) < kFull) {
        return site;
    }
    if (building.canopy > 0 && building.canopy < kFull) {
        return BuildKind::Canopy;
    }
    if (building.bedding > 0 && building.bedding < kFull) {
        return BuildKind::Bedding;
    }
    // Забор последним: на клетке края обжитого обычно нет ничего другого, а
    // если есть, то доделывать сперва стоит то, ради чего сюда ходят спать.
    if (building.fence > 0 && building.fence < kFull) {
        return BuildKind::Fence;
    }
    return BuildKind::None;
}

// Пора ли постройкам на этой клетке обветшать. Сдвиг по номеру клетки — чтобы
// весь лагерь не осыпался в один тик: срок один на всех, очередь у каждого
// своя. Тот же приём, что у ягод и троп.
inline bool buildDecays(std::uint64_t tick, std::size_t cell) {
    return (tick + static_cast<std::uint64_t>(cell)) % static_cast<std::uint64_t>(kBuildDecayPeriod) == 0;
}

// Материал под рукой: сколько силы наберётся из этого запаса.
inline int materialIn(const Resources& res) {
    return materialStrength(res.of(ResourceKind::Straw), res.of(ResourceKind::Twigs));
}

// Истратить на одну единицу труда. Тратится сперва солома: ветки берегутся на
// то, чего соломой не покроешь. Разницы для готовой постройки нет — прочность
// одна, — но пока лежит и то, и другое, разумнее класть в дело дешёвое.
//
// false — не хватило: столько материала здесь не наберётся.
inline bool spendMaterial(Resources& res) {
    if (materialIn(res) < kMaterialPerWork) {
        return false;
    }
    int spend = kMaterialPerWork;
    const int fromStraw = std::min(res.of(ResourceKind::Straw), spend);
    res.of(ResourceKind::Straw) -= fromStraw;
    spend -= fromStraw;
    while (spend > 0 && res.of(ResourceKind::Twigs) > 0) {
        --res.of(ResourceKind::Twigs);
        spend -= kTwigStrength;
    }
    return true;
}

// Единица труда: съесть материал, поднять прочность.
//
// Материал берётся СПЕРВА ИЗ КУЧИ на этой клетке и только потом из рук
// работающего, и в этом весь смысл склада при стройке: принёс и сложил —
// достроит кто угодно, в том числе пришедший с пустыми руками. Одни носят,
// другие строят, и совместный труд получается настоящим, а не двумя
// одиночками рядом.
//
// false — работать нечем: материала нет ни там, ни там. Это не ошибка, а
// обычное дело — принесут ещё.
inline bool applyWork(BuildKind kind, BuildingComponent& building, Resources* heap, Resources* hands) {
    if (kind == BuildKind::None) {
        return false;
    }
    int& condition = buildingCondition(building, kind);
    if (condition >= kFull) {
        return false;
    }
    if (heap != nullptr && spendMaterial(*heap)) {
        // взято из кучи
    } else if (hands != nullptr && spendMaterial(*hands)) {
        // взято из рук
    } else {
        return false;
    }
    condition = std::min(kFull, condition + conditionPerWork(kind));
    return true;
}

// Что этому месту нужнее. Ответ считается не признаками ("сыро — значит
// подстилка"), а тем, СКОЛЬКО КАЖДАЯ ПОСТРОЙКА ЗДЕСЬ ДОБАВИТ: закон отдыха
// уже знает про место всё, и спрашивать его второй раз другими словами было
// бы заведением второго закона.
//
// Отсюда само собой и получается правило "строят то, чего не хватает": на
// открытой поляне навес добавит четыреста, а под деревом — ничего: крыша
// одна, берётся лучшая.
// Прибавка возвращается вместе с видом, и это не удобство вызывающему, а
// единственный честный ответ на вопрос "насколько гоблина гонит строить".
//
// ПОД ДЕРЕВОМ ответ пустой, и это не мелочь округления. Прежде здесь стояло
// "там гоблин положит подстилку" — а placeSite под деревом отказывает и
// подстилке тоже (клетку занимает ствол), то есть два закона об одной клетке
// говорили разное. Пока позыв к стройке был почти всегда нулевым, расхождение
// молчало; стоило мерить позыв самой прибавкой — и гоблин с лежанкой под
// кроной захотел бы строить, поставить не смог бы и ушёл бы ломать ветки под
// замысел, которого не будет, навсегда.
//
// Забор дерева не боится (placeSite, underTreeAllowed), но его здесь и нет:
// этот закон отвечает на вопрос "чего не хватает ЗДЕСЬ", а кол улучшает не ту
// клетку, где стоишь, а ту, что за ним.
//
// Прежде нехватку мерили так: kRestGood минус годность места. Мерка выглядела
// разумной ровно до тех пор, пока в лагере не начали умирать: в годность
// входит штраф за лежащую рядом тушу (kRestCarcassPenalty, core/Rest.hpp),
// вычитается он сильно, а НИ ОДНА постройка его не убирает. Гоблин у свежей
// падали чувствовал нехватку под шесть сотен и брался строить то, чего
// падаль не отменяет, — и так до тех пор, пока туша не сгниёт. Пока зубов у
// мира не было, случалось это редко; теперь будет часто.
//
// Гонит же гоблина не то, что здесь плохо, а то, что он может здесь
// поправить. Разница выходит наружу только в местах, где плохое непоправимо,
// — и ровно там прежняя мерка и врала.
struct BuildChoice {
    BuildKind kind = BuildKind::None;
    int gain = 0;  // насколько поднимет годность, 0..kFull
};

inline BuildChoice betterBuild(const RestPlace& place) {
    if (place.tree) {
        return BuildChoice{};
    }
    const int roof = std::max(place.tree ? kRestShelter : 0,
                              kRestCanopy * std::clamp(place.canopy, 0, kFull) / kFull);
    const int canopyGain = std::max(0, kRestCanopy - roof);
    const int beddingGain = kRestBedding - kRestBedding * std::clamp(place.bedding, 0, kFull) / kFull;
    if (canopyGain <= 0 && beddingGain <= 0) {
        return BuildChoice{};
    }
    return canopyGain >= beddingGain ? BuildChoice{BuildKind::Canopy, canopyGain}
                                     : BuildChoice{BuildKind::Bedding, beddingGain};
}

// Положить площадку на клетку. Через очередь команд (05_Entity.md, п.5):
// добавление компонента — структурное изменение. Если площадка там уже есть,
// вторая не ставится: замысел на клетке один.
inline void placeSite(World& world, int x, int y, BuildKind kind) {
    if (kind == BuildKind::None || !world.area().inBounds(x, y)) {
        return;
    }
    // Забору дерево не помеха, в отличие от навеса и подстилки: плетень между
    // стволами — самое естественное, что бывает, и роща в краю обжитого иначе
    // давала бы вечную дыру, которую нечем закрыть. Оттого проверка ниже и
    // сделана по виду, а не общей на всех.
    const bool underTreeAllowed = kind == BuildKind::Fence;
    // Под деревом не строят: дерево занимает клетку целиком, и ни навесу, ни
    // подстилке там места нет. Проверка стоит в самом законе, а не только в
    // решении гоблина: закон, мимо которого можно пройти одной забытой
    // проверкой, рано или поздно обойдут.
    //
    // Следствие называется прямо: лагерь под рощей не обстраивается вовсе — он
    // и так хорош тенью, — а роща остаётся тем, чем была, местом, куда ходят за
    // ветками.
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!underTreeAllowed && world.registry().all_of<TreeComponent>(tile)) {
            return;
        }
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

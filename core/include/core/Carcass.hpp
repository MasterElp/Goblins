#pragma once

#include <algorithm>

#include "core/Body.hpp"
#include "core/CommandQueue.hpp"
#include "core/World.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/SoilComponent.hpp"

namespace goblins {

// Смерть и падаль — ОДИН закон мира на всё, что умирает своим телом.
//
// Живёт здесь, а не внутри AnimalSystem, по той же причине, что и
// depositHumus в core/Humus.hpp: пользуются им уже двое. Гоблин умирает тем
// же способом и ложится той же тушей, и разными эти два закона быть не
// могут — иначе в мире появятся два разных ответа на вопрос, сколько мяса
// остаётся от тела.

// Сколько мяса остаётся от взрослого животного. В единицах биомассы, тех
// же, что развитость растения: туша — это десяток кустов травы разом,
// поэтому охота и окупается. От детёныша остаётся меньше — мясо считается
// от размера.
//
// Число не косметическое, а решающее: одна добыча должна кормить хищника
// заметно дольше, чем длится охота на следующую. При втрое меньшем значении
// хищники вымирали за первую тысячу тиков — они успевали загнать жертву, но
// не успевали окупить погоню, и весь их род держался на том, чтобы ни разу
// не промахнуться.
constexpr int kMeatPerSize = 8000;

// Как быстро падаль пропадает сама. Несъеденная туша не лежит вечно: она
// гниёт, и накопленный зверем белок доходит до почвы перегноем, просто
// медленнее, чем через чей-то желудок.
//
// Гниёт быстро — тушу целиком за несколько сотен тиков. При медленном
// гниении мир получал "банк падали": когда стадо, объевшее луг, вымирало
// разом от голода, сотни туш лежали тысячи тиков и кормили хищников
// вшестеро против того, что могла прокормить живая добыча. Расплодившись на
// падали, хищники доедали уцелевшее стадо, и мир пустел совсем. Мясо должно
// портиться — иначе однажды случившийся мор кормит вечно.
//
// Гниение проходит РОВНО ОДИН раз за тик, и делает это AnimalSystem. Всякий,
// кто ест падаль, её только убавляет; гноить её второй системой значит
// гноить её вдвое быстрее, и заметить это будет нечем — туши просто станут
// пропадать раньше, чем до них доходят.
constexpr int kCarcassRot = 20;

// Падаль ложится на терраформирующий Entity тайла, рядом с почвой, водой и
// перегноем (см. CarcassComponent). Если туша там уже лежит, мясо и белок
// просто складываются: две смерти на одной клетке — это одна куча падали,
// а не две отдельные.
//
// Вызывать только из команды очереди (05_Entity.md, п.5): добавление
// компонента — структурное изменение.
inline void depositCarcass(World& world, int x, int y, int meat, int protein) {
    if ((meat <= 0 && protein <= 0) || !world.area().inBounds(x, y)) {
        return;
    }
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!world.registry().all_of<SoilComponent>(tile)) {
            continue;
        }
        if (auto* carcass = world.registry().try_get<CarcassComponent>(tile)) {
            carcass->meat += meat;
            carcass->protein += protein;
        } else {
            world.registry().emplace<CarcassComponent>(tile, CarcassComponent{meat, protein});
        }
        return;
    }
}

// Крупицы белка распределены по туше равномерно, поэтому убыль мяса —
// съеденного или сгнившего — освобождает их пропорционально. Куда они
// пойдут дальше (в едока или в перегной), решает вызывающая сторона: для
// самой туши это одно и то же убывание.
inline int releaseCarcassProtein(CarcassComponent& carcass, int meatBefore, int removed) {
    if (meatBefore <= 0 || removed <= 0 || carcass.protein <= 0) {
        return 0;
    }
    // Целочисленно и без накопителя: сколько крупиц приходится на
    // унесённую долю мяса, столько и освобождается. Округление вниз ничего
    // не теряет — неосвобождённое остаётся в туше и уйдёт со следующим
    // куском или с гниением.
    const int released = std::min(carcass.protein, carcass.protein * std::min(removed, meatBefore) / meatBefore);
    carcass.protein -= released;
    return released;
}

// Смерть существа — один путь для всех причин: старость, истощение,
// чужие зубы. Тело ложится падалью на ту клетку, где оно легло, и
// дальше его либо съедят, либо оно сгниёт в перегной.
inline void enqueueDeath(CommandQueue& commands, entt::entity entity, int x, int y) {
    commands.enqueue([entity, x, y](World& w) {
        if (!w.registry().valid(entity)) {
            return;
        }
        // Читаем состояние заново, а не полагаемся на снятое при постановке
        // команды: пока она ждала очереди, тик мог доработать
        // (05_Entity.md, п.5).
        int meat = 0;
        int protein = 0;
        if (const auto* body = w.registry().try_get<const AnimalComponent>(entity)) {
            meat = kMeatPerSize * bodySize(body->growth) / kFull;
            // И накопленный белок, и не вышедший навоз: из тела в мир
            // уходит всё, что в нём было.
            protein = body->protein + body->dung;
        }
        depositCarcass(w, x, y, meat, protein);
        w.despawn(entity);
    });
}

} // namespace goblins

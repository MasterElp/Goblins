#pragma once

#include <algorithm>
#include <cstdlib>

#include "core/Scale.hpp"
#include "core/components/KnowledgeComponent.hpp"

namespace goblins {

// Как гоблин помнит места — ОДИН закон на систему и на наблюдателя.
//
// Живёт здесь, а не внутри GoblinSystem, по той же причине, что и
// core/Rest.hpp: пользуются им уже двое. Система — чтобы решить, куда идти,
// когда нужного не видно; сервер — чтобы нарисовать помеченные места на
// карте вокруг наблюдаемого гоблина. Нарисованная память, не совпадающая с
// той, по которой он принял решение, хуже ненарисованной: по ней нельзя
// понять, почему он пошёл именно туда.

// Сколько твёрдости прибавляет одно удачное посещение. Место, которым
// пользуются постоянно, доходит до полного за пару десятков тиков — примерно
// столько и длится еда или водопой.
constexpr int kRememberGain = 40;

// Сколько твёрдости уходит за тик само. Целая единица без накопителя
// (core/Scale.hpp): при kFull = 1000 это значит, что место, к которому не
// возвращались тысячу тиков, забыто начисто.
//
// Забывание — не изнашивание и не оптимизация: это то, что заставляет
// возвращаться. Помни гоблин вечно, ему хватило бы одного обхода мира на всю
// жизнь, и никакого повторяющегося пути не возникло бы вовсе.
constexpr int kForgetRate = 1;

// Сколько твёрдости теряет место, на которое пришли и ничего не нашли.
// Заметно больше одного удачного посещения: объеденная поляна должна
// переставать быть едой быстрее, чем она ею становилась, иначе гоблин будет
// ходить к ней по памяти ещё сотни тиков.
constexpr int kDisappointLoss = 250;

// Во что обходится каждая клетка расстояния при выборе вспомненного места.
//
// Тем и отличается память от зрения, что вспомненное место может перевесить
// близкое: гоблин помнит, что за холмом вода, и идёт туда мимо ближайшей
// лужи, которой там нет. Но перевешивать оно должно не всегда, иначе гоблин
// будет ходить через полкарты к самому твёрдому воспоминанию, минуя всё по
// дороге.
constexpr int kRecallDistance = 4;

// С какой оценки вспомненное место побеждает УВИДЕННОЕ.
//
// Порог этот — вся разница между ночёвкой и лагерем, и без него шаг "места
// притяжения" сводится к слагаемому в сумме. Пока гоблин ложится на первой
// же годной клетке, какая попалась на глаза, возвращаться ему не к чему:
// годных клеток на карте много, и та же самая выпадает лишь по совпадению.
// Стоит же ему предпочесть ОБЖИТОЕ место годному — и место, к которому он
// уже приходил, начинает притягивать его снова, а дорога туда — набиваться.
//
// Половина полной твёрдости: при kRecallDistance = 4 это значит, что за
// обжитым местом гоблин вернётся шагов за сто, а за случайно запомненным —
// не пойдёт вовсе. Место становится обжитым не по названию, а по числу
// приходов: kRememberGain за раз, то есть дюжина возвращений.
constexpr int kRestReturn = 500;

// Запомнить (или укрепить) место. Вызывать тогда, когда место ПРИГОДИЛОСЬ, —
// не когда его увидели: помнить надо то, что помогло, а не то, что попалось
// на глаза.
inline void remember(KnowledgeComponent& mind, PlaceKind kind, int x, int y, int gain = kRememberGain) {
    if (kind == PlaceKind::None || gain <= 0) {
        return;
    }
    // Уже помним это самое место — просто твёрже.
    for (auto& place : mind.places) {
        if (place.kind == kind && place.x == x && place.y == y) {
            place.strength = std::min(kFull, place.strength + gain);
            return;
        }
    }
    // Свободный слот — или самый слабый, если свободных нет. Вытесняется
    // слабейшее из ВСЕГО, а не из мест того же вида: иначе один вид занял бы
    // голову целиком, и гоблин, знающий четыре кормовых участка, не смог бы
    // запомнить, где вода.
    KnownPlace* weakest = nullptr;
    for (auto& place : mind.places) {
        if (place.kind == PlaceKind::None || place.strength <= 0) {
            weakest = &place;
            break;
        }
        if (weakest == nullptr || place.strength < weakest->strength) {
            weakest = &place;
        }
    }
    if (weakest == nullptr) {
        return;
    }
    // Занять чужой слот можно только тем, что уже твёрже занятого: иначе
    // каждое случайное место выбивало бы из головы обжитое.
    if (weakest->kind != PlaceKind::None && weakest->strength > 0 && weakest->strength >= gain) {
        return;
    }
    *weakest = KnownPlace{x, y, kind, std::min(kFull, gain)};
}

// Забывание со временем. Зовётся раз в тик на каждого помнящего.
inline void forget(KnowledgeComponent& mind) {
    for (auto& place : mind.places) {
        if (place.kind == PlaceKind::None) {
            continue;
        }
        place.strength -= kForgetRate;
        if (place.strength <= 0) {
            place = KnownPlace{};
        }
    }
}

// Пришёл и не нашёл. Место не вычёркивается сразу: один пустой приход может
// быть и случайностью (куст объели перед самым носом), а вот три подряд —
// уже нет.
inline void disappoint(KnowledgeComponent& mind, PlaceKind kind, int x, int y) {
    for (auto& place : mind.places) {
        if (place.kind == kind && place.x == x && place.y == y) {
            place.strength -= kDisappointLoss;
            if (place.strength <= 0) {
                place = KnownPlace{};
            }
            return;
        }
    }
}

// Лучшее вспомненное место этого вида: твёрдость минус расстояние. Возвращает
// nullptr, если такого вида гоблин не помнит вовсе или если лучшее из
// вспомненного не дотянуло до minScore.
//
// Порог — не отсечка "плохих" мест, а способ спросить у памяти иначе: "есть
// ли место, ради которого стоит идти МИМО того, что видно". Без порога
// (minScore = 0) вопрос прежний — "помню ли я хоть что-нибудь".
//
// Расстояние — в шагах (восемь соседей, диагональ стоит столько же, сколько
// прямая), а не по прямой: гоблин ходит ногами.
inline const KnownPlace* recall(const KnowledgeComponent& mind, PlaceKind kind, int fromX, int fromY,
                                 int minScore = 0) {
    const KnownPlace* best = nullptr;
    int bestScore = 0;
    for (const auto& place : mind.places) {
        if (place.kind != kind || place.strength <= 0) {
            continue;
        }
        const int steps = std::max(std::abs(place.x - fromX), std::abs(place.y - fromY));
        const int score = place.strength - steps * kRecallDistance;
        if (score < minScore) {
            continue;
        }
        if (best == nullptr || score > bestScore) {
            best = &place;
            bestScore = score;
        }
    }
    return best;
}

} // namespace goblins

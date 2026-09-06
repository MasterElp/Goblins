#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "core/Bonds.hpp"
#include "core/Character.hpp"
#include "core/Knowledge.hpp"
#include "core/Random.hpp"
#include "core/Scale.hpp"

namespace goblins {

// К кому гоблин подойдёт поговорить и что из разговора выйдет — тот же
// закон и на систему, и на наблюдателя, что и выбор пары (core/Mating.hpp).
//
// Здесь только ВЫБОР. Сама встреча (кто с кем сошёлся, кто кого окликнул и
// что перешло из рук в руки) остаётся в GoblinSystem: это уже не знание
// гоблина, а событие мира — ровно то же разделение, что у пары.
//
// Разговор ищут глазами, а не зовом, в отличие от пары. Причина не в
// громкости: зов заведён затем, чтобы последние двое в мире могли встретиться
// и оставить потомство, — без него поголовье вымирало от одиночества. У
// болтовни такой цены нет. Гоблин, готовый идти через полкарты поговорить,
// перестал бы есть и работать, а поселение — а его-то и надо увидеть —
// возникло бы не из общих мест, а из общего разговора.

// На каком расстоянии разговор состоится. Вплотную, включая свою же клетку:
// пара сходится на одной клетке (там ей и место), а двое говорящих стоят
// рядом — так их и видно на карте, двумя фигурками, а не одной.
inline constexpr int kTalkRange = 1;

// Насколько вообще стоит подойти к кому бы то ни было. Основание, из которого
// потом вычитается дорога: без него ни один сосед не набрал бы ничего, и
// незнакомые не заговаривали бы вовсе.
inline constexpr int kTalkNear = 400;

// Во что обходится каждая клетка до собеседника. При зоркости гоблина (5..18)
// дальний край поля зрения съедает почти всё основание — значит, к дальнему
// идут только за теплом, а к соседу заглядывают просто так.
inline constexpr int kTalkDistance = 20;

// Насколько тянет к незнакомому — и тянет тем сильнее, чем НЕПОСТОЯННЕЕ нрав.
//
// Это вторая половина ответа на вопрос "с одними и теми же или со всеми
// подряд". Первая — тепло, умноженное на постоянство. Две тяги смотрят в
// разные стороны и делятся одним числом нрава, поэтому крайности получаются
// сами: постоянный ходит к своим мимо новых лиц, непостоянный здоровается со
// всеми и ни с кем не сближается. Заводить для этого второе число не
// пришлось.
inline constexpr int kTalkNovelty = 300;

// Во что обходится чужое племя. Цена, а не запрет: племена должны иметь
// возможность смешаться, иначе "расходятся или смешиваются" (шаг 9) решено
// за мир заранее. Но цена немалая — со своим при прочих равных заговорят
// охотнее, и связи внутри племени завяжутся плотнее, чем поперёк.
inline constexpr int kStrangerTribe = 250;

// С какой твёрдостью приезжает слух — при полной склонности слушателя.
//
// Ровно столько, сколько отнимает одно разочарование (kDisappointLoss,
// core/Knowledge.hpp), и это не совпадение: услышанное место обязано
// пережить РОВНО ОДНУ проверку. Сходил, нашёл — место подтвердилось своим
// ходом и дальше живёт как всякое обжитое; сходил, не нашёл — слух умер
// целиком, и второй раз гоблин туда не пойдёт.
//
// Меньше нельзя: при твёрдости ниже сотни recall не вспомнит место дальше
// двух десятков шагов (kRecallDistance), то есть слух не дошёл бы до ног
// вовсе. Больше нельзя: слух начал бы вытеснять из головы места, на которых
// гоблин бывал сам (правило вытеснения в remember), и чужие слова весили бы
// больше собственного опыта.
inline constexpr int kHearsayFull = 250;

// Сколько тепла переносит доброе слово о третьем (Topic::Kin).
//
// Вдвое слабее знакомства вживую (kWarmGain), и слабее намеренно: молва
// обязана знакомить, а не заменять знакомство. При этой величине доброе
// слово ложится только в пустую голову или поверх совсем остывшего (правило
// вытеснения в warmTo) — то есть заводит НОВОЕ имя, а тех, с кем гоблин
// действительно знаком, не трогает.
//
// Отсюда и всё, ради чего тема заведена: у обаятельного гоблина заводится
// вес, которого он сам никому не показывал, — его знают те, кого он не
// встречал. Это же и дверь к будущим просьбам: слушаются того, к кому тепло.
inline constexpr int kGoodWord = 30;

// Сам заговоривший: где стоит, докуда видит, кто он и насколько постоянен.
struct Talker {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    int perception = 1;
    int tribe = 0;
    int loyal = kFull / 2;
};

// Возможный собеседник — тем, что о нём видно со стороны.
//
// Согласия здесь нет, в отличие от пары (MateCandidate::willing), и это
// решение о мире, а не упрощение: заговорить можно и с занятым. Требуй
// разговор встречного желания у обоих — и он случался бы, только когда двое
// заскучали разом, то есть почти никогда, а трудягу нельзя было бы окликнуть
// за работой. Мёртвый в этот тик собеседником быть не может, и только это и
// проверяется.
struct Companion {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    int tribe = 0;
    bool alive = false;
};

struct TalkChoice {
    bool found = false;
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
};

// Расстояние шагами: восемь соседей, диагональ стоит столько же, сколько
// прямая, — гоблин ходит ногами (та же мера, что в recall).
inline int stepsBetween(int ax, int ay, int bx, int by) {
    return std::max(std::abs(ax - bx), std::abs(ay - by));
}

// Стоят ли двое достаточно близко, чтобы разговор состоялся.
inline bool withinTalk(int ax, int ay, int bx, int by) {
    return stepsBetween(ax, ay, bx, by) <= kTalkRange;
}

// Насколько хочется подойти именно к этому. Ноль и ниже — не хочется вовсе.
inline int talkAppeal(const Talker& talker, const Companion& companion, int warmth) {
    int appeal = kTalkNear - stepsBetween(talker.x, talker.y, companion.x, companion.y) * kTalkDistance;
    // Тянет к знакомому — тем сильнее, чем постояннее нрав.
    appeal += warmth * std::clamp(talker.loyal, 0, kFull) / kFull;
    // И тянет к новому лицу — тем сильнее, чем нрав непостояннее. "Новое" —
    // это именно незнакомое, а не холодное: остывший знакомый уже не в
    // новинку, ему просто не рады.
    if (warmth <= 0) {
        appeal += kTalkNovelty * (kFull - std::clamp(talker.loyal, 0, kFull)) / kFull;
    }
    if (companion.tribe != talker.tribe) {
        appeal -= kStrangerTribe;
    }
    return appeal;
}

// К кому пойти.
//
// Отдельной дешёвой проверки "есть ли вообще кто-то рядом" здесь нет —
// в отличие от пары, где такая проверка (anyMateInSight) стоит перед волной
// дороги и бережёт именно её. Волны здесь нет, беречь нечего, а две проверки
// вместо одной ответили бы по-разному: увидеть можно и того, к кому идти не
// стоит (дальний чужак дороже, чем разговор с ним), — и гоблин застрял бы в
// желании, которого не исполняет. Вопрос один: есть ли тот, к кому стоит
// подойти, — и отвечает на него один закон. Из равных по влечению побеждает меньший идентификатор, а не
// первый в списке: порядок в памяти не может быть причиной события в мире
// (02_CorePrinciples.md, п.12a).
inline TalkChoice chooseCompanion(const Talker& talker, std::span<const Companion> companions,
                                  const BondsComponent& bonds) {
    const int sight = std::max(1, talker.perception);
    TalkChoice choice;
    int bestAppeal = 0;
    for (const auto& companion : companions) {
        if (!companion.alive || companion.id == talker.id) {
            continue;
        }
        const int dx = companion.x - talker.x;
        const int dy = companion.y - talker.y;
        if (dx * dx + dy * dy > sight * sight) {
            continue;
        }
        const int appeal = talkAppeal(talker, companion, warmthTo(bonds, companion.id));
        if (appeal <= 0) {
            continue;
        }
        if (!choice.found || appeal > bestAppeal || (appeal == bestAppeal && companion.id < choice.id)) {
            choice = TalkChoice{true, companion.id, companion.x, companion.y};
            bestAppeal = appeal;
        }
    }
    return choice;
}

// О чём он заговорит.
//
// Тема выбирается жребием по склонностям, а не берётся сильнейшая: гоблин, у
// которого еда чуть интереснее воды, говорил бы о еде ВСЕГДА, и половина
// склонностей не значила бы ничего. Жребий же делает сильную склонность
// частой, а не единственной.
//
// Доступны при этом не все темы сразу, и обе отсечки — про мир, а не про
// удобство. Место можно назвать, только если оно есть в голове: пересказать
// то, чего не помнишь, нельзя. И только если собеседник достаточно свой
// (kNewsWarmth) — иначе первое же "здравствуй" раздавало бы карту мира.
inline Topic chooseTopic(const CharacterComponent& character, const KnowledgeComponent& mind,
                         const BondsComponent& bonds, int x, int y, int warmth, std::uint64_t& state) {
    std::array<int, kTopicCount> weight{};
    int total = 0;
    for (int slot = 0; slot < kTopicCount; ++slot) {
        const auto topic = static_cast<Topic>(slot);
        const PlaceKind place = placeOf(topic);
        int share = interestIn(character, topic);
        if (place != PlaceKind::None) {
            if (warmth < kNewsWarmth || recall(mind, place, x, y) == nullptr) {
                share = 0;
            }
        } else if (topic == Topic::Kin && closestFace(bonds) == nullptr) {
            // Отозваться не о ком.
            share = 0;
        }
        weight[static_cast<std::size_t>(slot)] = share;
        total += share;
    }
    // Сказать нечего и не о ком — значит, разговор ни о чём. Не запасной
    // случай, а самый обычный: таковы все первые разговоры в мире.
    if (total <= 0) {
        return Topic::Idle;
    }
    int roll = static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(total)));
    for (int slot = 0; slot < kTopicCount; ++slot) {
        roll -= weight[static_cast<std::size_t>(slot)];
        if (roll < 0) {
            return static_cast<Topic>(slot);
        }
    }
    return Topic::Idle;
}

// С какой твёрдостью услышанное место ляжет в голову слушателя.
//
// По склонности СЛУШАТЕЛЯ, а не говорящего: интересно не тому, кто говорит.
// Равнодушный (склонность 0) не удержит ничего, и это не потеря — это
// единственный способ сказать "в одно ухо влетело" в мире, где нет других
// слов. Слабый же слух (склонность мала) ляжет и растает раньше, чем гоблин
// успеет туда дойти, — тоже честный ответ, и получается он сам, без порога.
inline int hearsayGain(int interest) {
    return kHearsayFull * std::clamp(interest, 0, kFull) / kFull;
}

} // namespace goblins

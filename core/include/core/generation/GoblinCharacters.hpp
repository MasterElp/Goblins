#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/Random.hpp"
#include "core/Scale.hpp"
#include "core/components/CharacterComponent.hpp"

namespace goblins {

// Откуда берётся нрав — то же место в мире, что GoblinGenetics.hpp занимает
// для генома: общий для двух вызывающих сторон набор чистых функций
// (генерация расселяет первое поголовье, GoblinSystem порождает детей).
//
// Механика здесь СВОЯ, а не общая с геномом, и это главное решение файла.
// Геном раскладывает бюджет преимуществ: черта в нём покупается за другую
// черту, потому что вид не может быть лучше другого во всём. У нрава такого
// бюджета нет и быть не должно — общительный не обязан платить за это
// молчаливостью в чём-то ещё, он просто другой. Положи нрав в ту же
// механику, и мир начал бы утверждать, будто болтливость чего-то стоит.
//
// Общее с геномом осталось там, где оно про наследование, а не про цену:
// племя задаёт середину, особь отклоняется в полосе вокруг неё, и полоса эта
// одна и та же при расселении и при рождении. Оттого племя и не расползается
// за поколения — ровно тот же ответ, что kSpeciesBand даёт геному.

// Насколько племена расходятся между собой по каждому числу нрава, вокруг
// середины шкалы.
inline constexpr int kTribeSpread = 200;

// Насколько поднят конёк племени и опущено его слабое место.
//
// Без этой пометки два племени могли бы выпасть почти одинаковыми, и слова
// "у племени свой нрав" не значили бы ничего — та же беда, от которой геном
// защищается kMinSpeciesDistance. Но там она решается перебором попыток, а
// здесь — назначением по кругу: конёк и слабое место у каждого следующего
// племени свои, и совпасть они не могут, пока племён не больше, чем чисел
// нрава. Перебор с проверкой расстояния дал бы то же самое дороже.
inline constexpr int kTribeMark = 300;

// Насколько ребёнок отклоняется от среднего родителей, прежде чем его
// вернут в полосу племени.
//
// Число своё, а не доля от разброса особей: это разные вещи. Разброс говорит,
// насколько широко племя вообще бывает разным; дрейф — насколько непохожим на
// родителей может выйти ребёнок. Свяжи их одним числом — и, сузив племя,
// нельзя было бы оставить детей непохожими на родителей.
inline constexpr int kCharacterDrift = 80;

namespace detail {

// Число вокруг середины, жребием, в пределах half в обе стороны.
inline int wobble(std::uint64_t& state, int centre, int half) {
    if (half <= 0) {
        return std::clamp(centre, 0, kFull);
    }
    const int span = 2 * half + 1;
    const int drawn = centre - half + static_cast<int>(randomBelow(state, static_cast<std::uint64_t>(span)));
    return std::clamp(drawn, 0, kFull);
}

// Четыре числа нрава по порядку — чтобы назначать конёк и слабое место
// перебором, а не четырьмя ветками. Порядок здесь ничего не означает, кроме
// самого себя: он не уезжает ни в файл, ни на провод.
inline int* traitSlot(CharacterComponent& nature, int slot) {
    switch (slot) {
        case 0: return &nature.sociable;
        case 1: return &nature.loyal;
        case 2: return &nature.diligent;
        default: return &nature.charming;
    }
}

inline int traitAt(const CharacterComponent& nature, int slot) {
    switch (slot) {
        case 0: return nature.sociable;
        case 1: return nature.loyal;
        case 2: return nature.diligent;
        default: return nature.charming;
    }
}

inline constexpr int kTraitSlots = 4;

} // namespace detail

// Нравы племён: по одному архетипу на племя, у каждого свой конёк и своё
// слабое место. Длина списка обязана совпадать с длиной списка геномов
// (makeGoblinTribes) — оба лежат в GoblinTribesComponent и берутся одним и
// тем же номером племени.
inline std::vector<CharacterComponent> makeGoblinCharacters(int count, std::uint64_t seed) {
    std::vector<CharacterComponent> tribes;
    if (count <= 0) {
        return tribes;
    }
    // Своя струя случайности, не общая с геномом: иначе нрав племени был бы
    // жёстко привязан к его телу, и быстрое племя всегда оказывалось бы
    // болтливым.
    std::uint64_t state = mixSeed(seed, 0xC7A9AC7E211D1EEFull);
    const int traitTurn = static_cast<int>(randomBelow(state, detail::kTraitSlots));
    const int topicTurn = static_cast<int>(randomBelow(state, kTopicCount));

    tribes.reserve(static_cast<std::size_t>(count));
    for (int tribe = 0; tribe < count; ++tribe) {
        CharacterComponent nature;
        for (int slot = 0; slot < detail::kTraitSlots; ++slot) {
            *detail::traitSlot(nature, slot) = detail::wobble(state, kFull / 2, kTribeSpread);
        }
        // Конёк и слабое место — противоположные по кругу, чтобы племя не
        // вышло разом хорошим во всём или никаким во всём.
        const int strong = (traitTurn + tribe) % detail::kTraitSlots;
        const int weak = (traitTurn + tribe + detail::kTraitSlots / 2) % detail::kTraitSlots;
        int* strongTrait = detail::traitSlot(nature, strong);
        int* weakTrait = detail::traitSlot(nature, weak);
        *strongTrait = std::clamp(*strongTrait + kTribeMark, 0, kFull);
        *weakTrait = std::clamp(*weakTrait - kTribeMark, 0, kFull);

        for (int slot = 0; slot < kTopicCount; ++slot) {
            nature.interest[static_cast<std::size_t>(slot)] = detail::wobble(state, kFull / 2, kTribeSpread);
        }
        // И одна тема, до которой это племя особенно охоче.
        const auto loved = static_cast<std::size_t>((topicTurn + tribe) % kTopicCount);
        nature.interest[loved] = std::clamp(nature.interest[loved] + kTribeMark, 0, kFull);

        tribes.push_back(nature);
    }
    return tribes;
}

// Нрав одной особи: середина — нрав племени, разброс — spread (свойство мира,
// в тысячных шкалы; это ПОЛНАЯ ширина полосы, поэтому в каждую сторону
// уходит половина). spread = 0 — все в племени одинаковы, и это законный
// мир, а не вырожденный: с него удобно смотреть, что даёт сам нрав.
inline CharacterComponent spreadCharacter(const CharacterComponent& archetype, int spread,
                                          std::uint64_t& state) {
    const int half = std::max(0, spread) / 2;
    CharacterComponent nature;
    for (int slot = 0; slot < detail::kTraitSlots; ++slot) {
        *detail::traitSlot(nature, slot) = detail::wobble(state, detail::traitAt(archetype, slot), half);
    }
    for (int slot = 0; slot < kTopicCount; ++slot) {
        const auto at = static_cast<std::size_t>(slot);
        nature.interest[at] = detail::wobble(state, archetype.interest[at], half);
    }
    return nature;
}

// Нрав ребёнка: среднее родителей, дрейф — и назад в полосу своего племени.
//
// Возврат в полосу обязателен, и это прямой урок генома (kSpeciesBand,
// core/generation/Genetics.hpp): случайный дрейф без границы — это блуждание,
// и за сотню поколений он уводит поголовье к краям шкалы, где все либо
// молчуны, либо болтуны. С границей племя дрейфует ВНУТРИ своего нрава,
// оставаясь собой.
//
// Полоса та же самая, что и при расселении, и это не совпадение, а смысл
// одного числа: spread означает "насколько особи расходятся вокруг племени"
// — одинаково для рождённых в первый день мира и для рождённых на
// тысячном тике.
inline CharacterComponent crossCharacters(const CharacterComponent& mother, const CharacterComponent& father,
                                          const CharacterComponent& archetype, int spread,
                                          std::uint64_t& state) {
    const int half = std::max(0, spread) / 2;
    CharacterComponent child;
    for (int slot = 0; slot < detail::kTraitSlots; ++slot) {
        const int middle = (detail::traitAt(mother, slot) + detail::traitAt(father, slot)) / 2;
        const int centre = detail::traitAt(archetype, slot);
        *detail::traitSlot(child, slot) =
            std::clamp(detail::wobble(state, middle, kCharacterDrift), std::max(0, centre - half),
                       std::min(kFull, centre + half));
    }
    for (int slot = 0; slot < kTopicCount; ++slot) {
        const auto at = static_cast<std::size_t>(slot);
        const int middle = (mother.interest[at] + father.interest[at]) / 2;
        const int centre = archetype.interest[at];
        child.interest[at] = std::clamp(detail::wobble(state, middle, kCharacterDrift),
                                        std::max(0, centre - half), std::min(kFull, centre + half));
    }
    return child;
}

} // namespace goblins

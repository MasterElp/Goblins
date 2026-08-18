#pragma once

#include <algorithm>
#include <cmath>

#include <raylib.h>

// Цвет тайла — смешение почвенных параметров разными оттенками (общее
// для экранов "Генерация мира" и "Симуляция", чтобы карта выглядела
// одинаково в обоих режимах).
namespace TileColors {

inline Color lerp(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{static_cast<unsigned char>(a.r + (b.r - a.r) * t), static_cast<unsigned char>(a.g + (b.g - a.g) * t),
                 static_cast<unsigned char>(a.b + (b.b - a.b) * t), 255};
}

// Каменистость задаёт материал (земля или серый камень), влажность
// затемняет поверх, минералы добавляют золотистый отблеск. Оттенка
// утоптанной земли здесь больше нет: утрамбованности не стало в самой
// почве (core/components/SoilComponent.hpp), и красить нечего.
// mineralsFraction — уже нормализованная доля (0..1), см.
// mineralsFraction(int) ниже: сам SoilComponent.minerals — счётное целое,
// не доля, поэтому нормализация вынесена отдельно, а не в этот блендер.
inline Color soil(float moisture, float rockiness, float mineralsFraction) {
    static const Color dirt{101, 67, 33, 255};
    static const Color rock{132, 130, 124, 255};
    static const Color wet{40, 46, 38, 255};
    static const Color mineral{196, 168, 62, 255};

    Color c = lerp(dirt, rock, rockiness);
    c = lerp(c, wet, moisture * 0.6f);
    c = lerp(c, mineral, mineralsFraction * 0.5f);
    return c;
}

// Насыщение цвета — с этого количества минералов на тайле дальнейший рост
// уже не меняет оттенок. Не привязано к TerrainParams::mineralsAverage
// (тот — про генерацию, это — чисто про то, где видимая шкала выходит на
// плато), поэтому отдельная константа, а не общий параметр.
constexpr float kMineralsVisualCap = 30.0f;

inline float mineralsFraction(int minerals) {
    return std::clamp(static_cast<float>(minerals) / kMineralsVisualCap, 0.0f, 1.0f);
}

// Цвета видов травы — по индексу вида (PlantGenomeComponent::species,
// приходит в снапшоте). Все оттенки зелёно-жёлто-оливковые: на карте это
// должно читаться как растительность, а не как отдельные объекты, но
// соседние виды при этом различимы. Двенадцать — ровно верхний предел
// числа видов в мире (core::kMaxGrassSpecies); если сервер когда-нибудь
// пришлёт индекс больше, цвет просто повторится (остаток от деления), а
// не приведёт к выходу за границы.
inline Color plantSpecies(int species) {
    static const Color palette[12] = {
        {74, 140, 54, 255},  {126, 168, 48, 255}, {46, 118, 82, 255},  {158, 176, 62, 255},
        {96, 152, 96, 255},  {60, 128, 118, 255}, {138, 148, 40, 255}, {84, 112, 44, 255},
        {112, 176, 88, 255}, {52, 96, 60, 255},   {170, 190, 96, 255}, {70, 150, 130, 255},
    };
    if (species < 0) {
        return palette[0];
    }
    return palette[species % 12];
}

// Трава поверх почвы — не отдельный квадрат, а подмешивание цвета вида:
// молодой проросток почти не виден на фоне земли, взрослое растение
// закрывает её почти целиком. При маленьком размере тайла (вся карта в
// окне на экране генерации) отдельный значок был бы неразличим, а
// подмешивание работает при любом масштабе.
inline Color plant(Color soilColor, int species, float growth) {
    return lerp(soilColor, plantSpecies(species), 0.35f + 0.55f * std::clamp(growth, 0.0f, 1.0f));
}

// Семя в клетке — тёмная крапина цвета своего вида: оно лежит В земле, а
// не стоит на ней, поэтому цвет вида сперва притемняется и только потом
// подмешивается к почве. Так семя не путается с проростком (тот уже
// зелёный и тем ярче, чем больше вырос), но видно, что клетка не пустая:
// на заросшем лугу семена лежат под своими родителями и на карте не
// видны вовсе — а вот семя, пережившее растение и ждущее в опустевшей
// клетке, увидеть важно.
inline Color seed(Color soilColor, int species) {
    static const Color earth{28, 24, 18, 255};
    return lerp(soilColor, lerp(plantSpecies(species), earth, 0.55f), 0.45f);
}

// Цвета видов хищников — по индексу вида. Палитра третья и намеренно
// холодно-красная: хищника надо отличать от добычи мгновенно и не путать
// ни с травой, ни с ней. Шесть — верхний предел числа видов хищников
// (core::kMaxPredatorSpecies).
inline Color predatorSpecies(int species) {
    static const Color palette[6] = {
        {198, 74, 60, 255}, {150, 56, 74, 255},  {226, 106, 78, 255},
        {124, 60, 92, 255}, {176, 92, 108, 255}, {214, 132, 120, 255},
    };
    if (species < 0) {
        return palette[0];
    }
    return palette[species % 6];
}

// Цвета видов травоядных — по индексу вида, как и у травы, но палитра
// намеренно другая: тёплые охристо-рыжие тона против зелени луга. Животное
// на карте — не часть растительности, и путать их взглядом нельзя. Восемь
// — верхний предел числа видов травоядных в мире
// (core::kMaxHerbivoreSpecies); индекс больше просто повторит цвет
// (остаток от деления), а не выйдет за границы.
inline Color herbivoreSpecies(int species) {
    static const Color palette[8] = {
        {214, 158, 84, 255},  {186, 116, 66, 255}, {231, 199, 122, 255}, {158, 96, 62, 255},
        {236, 172, 140, 255}, {198, 140, 92, 255}, {172, 128, 104, 255}, {242, 214, 178, 255},
    };
    if (species < 0) {
        return palette[0];
    }
    return palette[species % 8];
}

// Падаль — красновато-бурое пятно поверх почвы, заметно теплее перегноя:
// туша на земле должна читаться как событие, а не как оттенок грунта.
// Насыщенность растёт с количеством мяса и выходит на плато — разница
// между одной тушей и тремя на одном тайле уже не важна.
inline Color carcass(Color soilColor, float meat) {
    static const Color rot{122, 52, 44, 255};
    return lerp(soilColor, rot, std::clamp(meat / 6.0f, 0.0f, 1.0f) * 0.75f);
}

// Встревоженная земля — холодная лиловая дымка поверх почвы: место, где
// недавно охотились (DangerComponent). Цвет намеренно не красный и не
// бурый — иначе он читался бы как та же падаль; тревога не вещество на
// земле, а её память, и выглядеть должна иначе. Доля 0..1 приходит с
// сервера уже готовой, поэтому и плато никакого не нужно.
inline Color danger(Color soilColor, float level) {
    static const Color dread{104, 66, 132, 255};
    return lerp(soilColor, dread, std::clamp(level, 0.0f, 1.0f) * 0.55f);
}

// Перегной — тёмное органическое пятно на почве. Насыщенность растёт с
// количеством минералов, которые ещё не вернулись в почву, и выходит на
// плато: разница между "тут умерло одно растение" и "тут умерло десять"
// после какого-то предела уже не читается.
inline Color humus(Color soilColor, int minerals) {
    static const Color rot{58, 44, 30, 255};
    return lerp(soilColor, rot, std::clamp(static_cast<float>(minerals) / 12.0f, 0.0f, 1.0f) * 0.6f);
}

// От мелкой (светлее, бирюзовее) до глубокой (тёмно-синяя) воды.
inline Color water(float depth) {
    static const Color shallow{90, 150, 175, 235};
    static const Color deep{18, 36, 66, 255};
    const float t = std::clamp(depth / 3.0f, 0.0f, 1.0f);
    return lerp(shallow, deep, t);
}

// Рельефный шейдинг — не отдельный цвет, а множитель поверх уже
// смешанного цвета тайла (soil()/water()): низины темнее, возвышенности
// светлее. normalizedHeight — 0..1, нормализовано вызывающей стороной по
// min/max текущей карты (у HeightComponent.height нет фиксированного
// диапазона — он зависит от параметров генерации).
//
// Корень перед растяжением — не украшение, а поправка на форму рельефа:
// высота растёт от шума с нажимом (kMountainSharpness в
// TerrainGenerator.cpp), поэтому бо́льшая часть карты сидит у нижнего края
// диапазона. Без поправки почти вся карта красилась бы одинаково тёмным, и
// чем выше горы, тем хуже: весь рельеф низин сминался бы в пару оттенков.
// Корень растягивает как раз низкую половину, где и лежит основная часть
// карты.
//
// Диапазон множителя широкий (0.45..1.5 против прежних 0.7..1.3): перепад
// высот в мире вырос в десятки раз, и прежнего размаха было мало, чтобы
// гору было видно горой.
inline Color applyHeightShading(Color c, float normalizedHeight) {
    const float t = std::sqrt(std::clamp(normalizedHeight, 0.0f, 1.0f));
    const float factor = 0.45f + 1.05f * t;
    return Color{static_cast<unsigned char>(std::clamp(c.r * factor, 0.0f, 255.0f)),
                 static_cast<unsigned char>(std::clamp(c.g * factor, 0.0f, 255.0f)),
                 static_cast<unsigned char>(std::clamp(c.b * factor, 0.0f, 255.0f)), c.a};
}

} // namespace TileColors

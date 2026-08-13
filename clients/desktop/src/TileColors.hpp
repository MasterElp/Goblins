#pragma once

#include <algorithm>

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

// Каменистость и утрамбованность задают материал (серый/утоптанная
// земля), влажность затемняет поверх, минералы добавляют золотистый
// отблеск. mineralsFraction — уже нормализованная доля (0..1), см.
// mineralsFraction(int) ниже: сам SoilComponent.minerals — счётное целое,
// не доля, поэтому нормализация вынесена отдельно, а не в этот блендер.
inline Color soil(float moisture, float rockiness, float compaction, float mineralsFraction) {
    static const Color dirt{101, 67, 33, 255};
    static const Color rock{132, 130, 124, 255};
    static const Color packed{150, 132, 96, 255};
    static const Color wet{40, 46, 38, 255};
    static const Color mineral{196, 168, 62, 255};

    Color c = lerp(dirt, rock, rockiness);
    c = lerp(c, packed, compaction * (1.0f - rockiness * 0.5f));
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
inline Color applyHeightShading(Color c, float normalizedHeight) {
    const float factor = 0.7f + 0.6f * std::clamp(normalizedHeight, 0.0f, 1.0f);
    return Color{static_cast<unsigned char>(std::clamp(c.r * factor, 0.0f, 255.0f)),
                 static_cast<unsigned char>(std::clamp(c.g * factor, 0.0f, 255.0f)),
                 static_cast<unsigned char>(std::clamp(c.b * factor, 0.0f, 255.0f)), c.a};
}

} // namespace TileColors

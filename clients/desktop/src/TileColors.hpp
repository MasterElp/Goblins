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
// земля), влажность затемняет поверх.
inline Color soil(float moisture, float rockiness, float compaction) {
    static const Color dirt{101, 67, 33, 255};
    static const Color rock{132, 130, 124, 255};
    static const Color packed{150, 132, 96, 255};
    static const Color wet{40, 46, 38, 255};

    Color c = lerp(dirt, rock, rockiness);
    c = lerp(c, packed, compaction * (1.0f - rockiness * 0.5f));
    c = lerp(c, wet, moisture * 0.6f);
    return c;
}

// От мелкой (светлее, бирюзовее) до глубокой (тёмно-синяя) воды.
inline Color water(float depth) {
    static const Color shallow{90, 150, 175, 235};
    static const Color deep{18, 36, 66, 255};
    const float t = std::clamp(depth / 3.0f, 0.0f, 1.0f);
    return lerp(shallow, deep, t);
}

} // namespace TileColors

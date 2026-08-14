#pragma once

#include <cstdint>
#include <vector>

#include <raylib.h>

#include "NetworkClient.hpp"

// Карта мира одной текстурой: один тексель — один тайл.
//
// Раньше каждый экран рисовал карту вызовом DrawRectangle на тайл, то
// есть десятью тысячами вызовов на кадр (и это при том, что состояние
// мира меняется в разы реже, чем кадры). Здесь цвета тайлов считаются
// только когда действительно есть что пересчитывать — пришёл новый
// снимок состояния (WorldState::version) или игрок переключил слои, — а
// каждый кадр карта выводится одним DrawTexturePro.
namespace MapTexture {

// Какие слои участвуют в цвете тайла. Выключенный слой считается
// нулевым (см. TileColors::soil); высота — не часть смешения, а
// множитель яркости поверх готового цвета, вода делит выключатель с
// влажностью (вода на тайле и есть источник его влажности).
struct Layers {
    bool rockiness = true;
    bool compaction = true;
    bool moisture = true;
    bool minerals = true;
    bool height = true;
    bool plants = true;
};

inline bool operator==(const Layers& a, const Layers& b) {
    return a.rockiness == b.rockiness && a.compaction == b.compaction && a.moisture == b.moisture &&
           a.minerals == b.minerals && a.height == b.height && a.plants == b.plants;
}
inline bool operator!=(const Layers& a, const Layers& b) {
    return !(a == b);
}

// Живёт столько же, сколько экран, который её показывает (обычно —
// статическая переменная в его draw). Текстура намеренно не выгружается
// в деструкторе: экраны переживают окно, и UnloadTexture после
// CloseWindow обращался бы к уже уничтоженному GL-контексту, а при
// закрытии окна raylib освобождает свои ресурсы сам.
class Cache {
public:
    // Готовая текстура карты размером areaWidth x areaHeight текселей.
    // Вызывать только когда мир получен (state.areaWidth > 0) и окно уже
    // создано.
    const Texture2D& texture(const WorldState& state, const Layers& layers);

private:
    void rebuildPixels(const WorldState& state, const Layers& layers);

    Texture2D texture_{};
    std::vector<Color> pixels_;
    bool loaded_ = false;
    int builtWidth_ = 0;
    int builtHeight_ = 0;
    std::uint64_t builtVersion_ = 0;
    Layers builtLayers_{};
};

} // namespace MapTexture

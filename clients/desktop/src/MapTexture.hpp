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
// только когда действительно есть что пересчитывать — изменилась хоть одна
// клетка (WorldState::mapVersion) или игрок переключил слои, — а каждый кадр
// карта выводится одним DrawTexturePro.
//
// Именно КЛЕТКА, а не "пришло новое сообщение": пересчёт идёт по всему миру и
// внутри кадра, а нажатие паузы, уведомление и список миров не меняют в
// картинке карты ровно ничего.
namespace MapTexture {

// Какие слои участвуют в цвете тайла. Выключенный слой считается
// нулевым (см. TileColors::soil); высота — не часть смешения, а
// множитель яркости поверх готового цвета, вода делит выключатель с
// влажностью (вода на тайле и есть источник его влажности).
struct Layers {
    bool rockiness = true;
    // Тропы — свой выключатель, а не часть каменистости: смотреть на мир
    // без троп надо ровно затем, чтобы увидеть, где они появились.
    bool trampled = true;
    bool moisture = true;
    bool minerals = true;
    bool height = true;
    bool plants = true;
    // Животные рисуются значками поверх текстуры (см. WorldScreen), но их
    // выключатель влияет и на неё: падаль — состояние тайла, и гасить её
    // надо вместе со зверями, а не вместе с травой.
    bool animals = true;
    // Гоблины — свой выключатель, а не часть звериного: смотреть на мир без
    // поселенцев и смотреть на мир без зверья — разные надобности. Значков
    // на карте он касается, а текстуры — нет: след ноги гоблина живёт в
    // своём слое (trampled выше) и гаснет вместе с ним, потому что тропа
    // остаётся в земле и после того, как её набивший умер.
    bool goblins = true;
};

inline bool operator==(const Layers& a, const Layers& b) {
    return a.rockiness == b.rockiness && a.trampled == b.trampled && a.moisture == b.moisture &&
           a.minerals == b.minerals && a.height == b.height && a.plants == b.plants &&
           a.animals == b.animals && a.goblins == b.goblins;
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

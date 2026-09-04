#include "SpriteAtlas.hpp"

#include <algorithm>
#include <cstdio>

#include <rlgl.h>

namespace SpriteAtlas {

namespace {

// Ширина общего листа. Две тысячи с лишним — не предел видеокарты (на
// проверенной машине он 16384), а величина, при которой лист выходит почти
// квадратным: всё, что клиент рисует сегодня, укладывается примерно в
// девятьсот пятьдесят строк.
//
// Кадр шире листа положить некуда, и это единственное настоящее ограничение;
// самый широкий кадр сегодня — дерево крупно, тридцать два пикселя.
constexpr int kPageWidth = 2048;

// Выше этого лист не растёт. Не догадка о железе, а страховка от рисунка,
// который однажды заведут вдесятеро больше нынешних: не поместившийся блок
// просто не кладётся, рисунок оказывается негоден (complete == false), и
// вызывающая сторона рисует тем же, чем рисует без картинки вовсе. Молчаливой
// порчи чужих кадров тут случиться не может.
constexpr int kPageMaxHeight = 2048;

Color inkOf(const Palette& palette, char symbol) {
    for (const auto& ink : palette) {
        if (ink.symbol == symbol) {
            return ink.color;
        }
    }
    return Color{0, 0, 0, 0};
}

} // namespace

Rectangle Placement::source(int palette, int frame) const {
    if (!ready || columns <= 0) {
        return Rectangle{0, 0, 0, 0};
    }
    const int p = ((palette % palettes) + palettes) % palettes;
    const int f = ((frame % frames) + frames) % frames;
    // Раскраска идёт целым куском кадров, а не строкой листа: строки блока
    // заворачиваются, и одна раскраска может занять их несколько.
    const int cell = p * frames + f;
    const int column = cell % columns;
    const int row = cell / columns;
    return Rectangle{static_cast<float>(x + column * frameWidth),
                     static_cast<float>(y + row * frameHeight), static_cast<float>(frameWidth),
                     static_cast<float>(frameHeight)};
}

Placement Page::place(const Assets::SpriteSheet& art, std::span<const Palette> palettes) {
    Placement out;
    if (art.empty() || palettes.empty() || art.width <= 0 || art.height <= 0) {
        return out;
    }
    if (art.width > kPageWidth) {
        std::fprintf(stderr, "SpriteAtlas: кадр шире общего листа (%d > %d), рисунок не положен.\n",
                     art.width, kPageWidth);
        return out;
    }

    const int frames = static_cast<int>(art.frames.size());
    const int palettesCount = static_cast<int>(palettes.size());
    const int cells = frames * palettesCount;
    const int columns = std::max(1, std::min(cells, kPageWidth / art.width));
    const int rows = (cells + columns - 1) / columns;
    if (height_ + rows * art.height > kPageMaxHeight) {
        std::fprintf(stderr, "SpriteAtlas: общий лист переполнен, рисунок не положен.\n");
        return out;
    }

    // Блок кладётся новой полосой во всю ширину листа, а не подбирается к
    // свободному месту сбоку. Хвост последней строки блока при этом пропадает,
    // но пропадает предсказуемо: сегодня весь лист — около девятисот
    // пятидесяти строк из двух тысяч, и укладка похитрее сэкономила бы место,
    // которого и так вдвое больше нужного, ценой того, что положение блока
    // перестало бы зависеть только от порядка укладки.
    out.x = 0;
    out.y = height_;
    out.frameWidth = art.width;
    out.frameHeight = art.height;
    out.columns = columns;
    out.frames = frames;
    out.palettes = palettesCount;
    out.ready = true;

    height_ += rows * art.height;
    pixels_.resize(static_cast<std::size_t>(kPageWidth) * height_, Color{0, 0, 0, 0});

    for (int p = 0; p < palettesCount; ++p) {
        for (int f = 0; f < frames; ++f) {
            const auto& frame = art.frames[static_cast<std::size_t>(f)];
            const int cell = p * frames + f;
            const int originX = out.x + (cell % columns) * art.width;
            const int originY = out.y + (cell / columns) * art.height;
            for (int y = 0; y < art.height; ++y) {
                const std::string& row = frame[static_cast<std::size_t>(y)];
                for (int x = 0; x < art.width; ++x) {
                    const Color color =
                        inkOf(palettes[static_cast<std::size_t>(p)], row[static_cast<std::size_t>(x)]);
                    if (color.a == 0) {
                        continue;
                    }
                    pixels_[static_cast<std::size_t>(originY + y) * kPageWidth + originX + x] = color;
                }
            }
        }
    }

    // Заливка отложена до texture(): рисунки кладутся лениво, и за один кадр
    // на лист может лечь несколько блоков подряд. Заливать после каждого
    // значило бы отправлять весь лист на видеокарту столько раз, сколько на
    // нём рисунков, — замерено, пятнадцать заливок вместо одной обходились в
    // 72 мс против 19.
    dirty_ = true;
    return out;
}

const Texture2D& Page::texture() {
    if (dirty_) {
        upload();
        dirty_ = false;
    }
    return texture_;
}

void Page::upload() {
    // Всё, что уже подано в пачку raylib, но ещё не ушло на видеокарту,
    // ссылается на СТАРЫЙ номер текстуры. Рисунки кладутся лениво, то есть
    // посреди кадра, и без этого сброса первый же кадр, в котором на лист лёг
    // новый блок, рисовал бы всё поданное до него уничтоженной текстурой.
    if (loaded_) {
        rlDrawRenderBatchActive();
        UnloadTexture(texture_);
        loaded_ = false;
    }
    const Image image{pixels_.data(), kPageWidth, height_, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    texture_ = LoadTextureFromImage(image);
    // Рисунок пиксельный: сглаживание превратило бы его в пятно на любом
    // масштабе, кроме единичного.
    SetTextureFilter(texture_, TEXTURE_FILTER_POINT);
    loaded_ = true;
}

Page& page() {
    static Page single;
    return single;
}

Rectangle Baked::source(int palette, int frame) const {
    const int index = frame < 0 || frame >= static_cast<int>(frames.size()) ? 0 : frame;
    return placement.source(palette, frames.empty() ? 0 : frames[static_cast<std::size_t>(index)]);
}

bool Detailed::ready(Detail detail) const {
    return sheet(detail).complete;
}

const Baked& Detailed::sheet(Detail detail) const {
    return detail == Detail::Fine ? fine : coarse;
}

const Texture2D& Detailed::texture(Detail detail) const {
    return sheet(detail).texture();
}

Detail Detailed::detailFor(float tileSize) const {
    // Порог — ширина кадра крупного рисунка, а не выписанное число: заведи
    // кто-нибудь рисунок другого размера, и число разъехалось бы с ним молча.
    if (!fine.complete || tileSize < static_cast<float>(fine.placement.frameWidth)) {
        return Detail::Coarse;
    }
    return Detail::Fine;
}

Baked bake(const std::string& art, std::span<const Palette> palettes,
           std::span<const char* const> frameNames) {
    Baked out;
    out.frames.assign(frameNames.size(), -1);

    const Assets::SpriteSheet& picture = Assets::sprites(art);
    if (picture.empty()) {
        return out;
    }
    out.placement = page().place(picture, palettes);
    if (!out.placement.ready) {
        return out;
    }

    out.complete = true;
    for (std::size_t i = 0; i < frameNames.size(); ++i) {
        out.frames[i] = picture.frameIndex(frameNames[i]);
        if (out.frames[i] < 0) {
            out.complete = false;
        }
    }
    return out;
}

Detailed bakeDetailed(const std::string& art, std::span<const Palette> palettes,
                      std::span<const char* const> frameNames) {
    Detailed out;
    out.coarse = bake(art, palettes, frameNames);
    out.fine = bake(art + "_fine", palettes, frameNames);
    return out;
}

} // namespace SpriteAtlas

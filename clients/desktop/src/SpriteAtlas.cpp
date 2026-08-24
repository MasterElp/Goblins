#include "SpriteAtlas.hpp"

namespace SpriteAtlas {

namespace {

Color inkOf(const Palette& palette, char symbol) {
    for (const auto& ink : palette) {
        if (ink.symbol == symbol) {
            return ink.color;
        }
    }
    return Color{0, 0, 0, 0};
}

} // namespace

void Sheet::build(const Assets::SpriteSheet& art, std::span<const Palette> palettes) {
    ready_ = false;
    if (art.empty() || palettes.empty()) {
        return;
    }

    width_ = art.width;
    height_ = art.height;
    frames_ = static_cast<int>(art.frames.size());
    palettes_ = static_cast<int>(palettes.size());

    const int atlasWidth = frames_ * width_;
    const int atlasHeight = palettes_ * height_;
    std::vector<Color> pixels(static_cast<std::size_t>(atlasWidth) * atlasHeight, Color{0, 0, 0, 0});

    for (int p = 0; p < palettes_; ++p) {
        for (int f = 0; f < frames_; ++f) {
            const auto& frame = art.frames[static_cast<std::size_t>(f)];
            for (int y = 0; y < height_; ++y) {
                const std::string& row = frame[static_cast<std::size_t>(y)];
                for (int x = 0; x < width_; ++x) {
                    const Color color = inkOf(palettes[static_cast<std::size_t>(p)], row[static_cast<std::size_t>(x)]);
                    if (color.a == 0) {
                        continue;
                    }
                    const std::size_t atlasX = static_cast<std::size_t>(f) * width_ + x;
                    const std::size_t atlasY = static_cast<std::size_t>(p) * height_ + y;
                    pixels[atlasY * atlasWidth + atlasX] = color;
                }
            }
        }
    }

    Image image{};
    image.data = pixels.data();
    image.width = atlasWidth;
    image.height = atlasHeight;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    texture_ = LoadTextureFromImage(image);
    // Рисунок пиксельный: сглаживание превратило бы его в пятно на любом
    // масштабе, кроме единичного.
    SetTextureFilter(texture_, TEXTURE_FILTER_POINT);
    ready_ = true;
}

Rectangle Sheet::source(int palette, int frame) const {
    if (!ready_) {
        return Rectangle{0, 0, 0, 0};
    }
    const int p = ((palette % palettes_) + palettes_) % palettes_;
    const int f = ((frame % frames_) + frames_) % frames_;
    return Rectangle{static_cast<float>(f * width_), static_cast<float>(p * height_), static_cast<float>(width_),
                     static_cast<float>(height_)};
}

} // namespace SpriteAtlas

#include "MapTexture.hpp"

#include "TileColors.hpp"

namespace MapTexture {

void Cache::rebuildPixels(const WorldState& state, const Layers& layers) {
    const std::size_t cellCount = static_cast<std::size_t>(state.areaWidth) * state.areaHeight;
    pixels_.assign(cellCount, Color{0, 0, 0, 255});

    // Диапазон высот — для нормализации в TileColors::applyHeightShading.
    // Не пересчитывается здесь: state.heightMin/heightMax зафиксированы на
    // момент последнего world_init (NetworkClient.cpp), а не берутся живым
    // minmax по текущему массиву — иначе эрозия, подъедающая вершины
    // каждый тик, сужала бы диапазон и заставляла бы весь остальной рельеф
    // мерцать светлее вслед за одной проседающей горой, хотя высота
    // конкретной клетки не менялась вовсе.
    float minHeight = 0.0f;
    float heightRange = 0.0f;
    if (layers.height) {
        minHeight = state.heightMin;
        heightRange = state.heightMax - state.heightMin;
    }

    for (std::size_t i = 0; i < cellCount; ++i) {
        Color color = TileColors::soil(layers.moisture ? state.moisture[i] : 0.0f,
                                       layers.rockiness ? state.rockiness[i] : 0.0f,
                                       layers.minerals ? TileColors::mineralsFraction(state.minerals[i]) : 0.0f);
        if (layers.height && heightRange > 0.0f) {
            color = TileColors::applyHeightShading((state.height[i] - minHeight) / heightRange);
        }
        // Перегной — под травой: на клетке может быть и то, и другое
        // (семя охотно прорастает там, где перегной возвращает минералы в
        // почву), и тогда сверху видна именно трава.
        if (layers.plants && state.humus[i] > 0) {
            color = TileColors::humus(color, state.humus[i]);
        }
        // Встревоженность — под падалью и под травой: это самый нижний из
        // следов, память земли о том, что здесь рвали.
        if (layers.danger && !state.danger.empty() && state.danger[i] > 0.0f) {
            color = TileColors::danger(color, state.danger[i]);
        }
        // Падаль — поверх перегноя и под травой: туша лежит на земле, а
        // трава вокруг неё продолжает расти.
        if (layers.animals && !state.carcass.empty() && state.carcass[i] > 0.0f) {
            color = TileColors::carcass(color, state.carcass[i]);
        }
        // Семя — под травой, как и перегной: чаще всего оно и лежит под
        // своим родителем, и тогда сверху видно именно растение. Само
        // семя видно там, где клетка пуста, — то есть там, где оно ждёт
        // своего часа.
        if (layers.plants && state.seedSpeciesAt[i] >= 0) {
            color = TileColors::seed(color, state.seedSpeciesAt[i]);
        }
        if (layers.plants && state.plantSpeciesAt[i] >= 0) {
            color = TileColors::plant(color, state.plantSpeciesAt[i], state.plantGrowth[i]);
        }
        // Вода — не полупрозрачный слой поверх, а готовый цвет тайла
        // (TileColors::water непрозрачен), поэтому просто заменяет
        // предыдущий.
        if (layers.moisture && state.waterDepth[i] > 0.0f) {
            color = TileColors::water(state.waterDepth[i]);
        }
        pixels_[i] = color;
    }
}

const Texture2D& Cache::texture(const WorldState& state, const Layers& layers) {
    const bool sizeMatches = loaded_ && builtWidth_ == state.areaWidth && builtHeight_ == state.areaHeight;
    if (sizeMatches && builtVersion_ == state.version && builtLayers_ == layers) {
        return texture_;
    }

    rebuildPixels(state, layers);
    builtVersion_ = state.version;
    builtLayers_ = layers;

    if (sizeMatches) {
        UpdateTexture(texture_, pixels_.data());
        return texture_;
    }

    if (loaded_) {
        UnloadTexture(texture_);
        loaded_ = false;
    }
    const Image image{pixels_.data(), state.areaWidth, state.areaHeight, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    texture_ = LoadTextureFromImage(image);
    // Тайл — это тексель, растянутый до размера тайла на экране:
    // сглаживание размыло бы границы тайлов в кашу.
    SetTextureFilter(texture_, TEXTURE_FILTER_POINT);
    loaded_ = true;
    builtWidth_ = state.areaWidth;
    builtHeight_ = state.areaHeight;
    return texture_;
}

} // namespace MapTexture

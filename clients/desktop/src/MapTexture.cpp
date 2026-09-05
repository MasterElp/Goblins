#include "MapTexture.hpp"

#include <cstdint>

#include "TileColors.hpp"

namespace MapTexture {

Color Cache::colorAt(const WorldState& state, const Layers& layers, std::size_t i) {
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

    {
        Color color = TileColors::soil(layers.moisture ? state.moisture[i] : 0.0f,
                                       layers.rockiness ? state.rockiness[i] : 0.0f,
                                       layers.minerals ? TileColors::mineralsFraction(state.minerals[i]) : 0.0f,
                                       layers.trampled && i < state.trampled.size() ? state.trampled[i] : 0.0f);
        if (layers.height && heightRange > 0.0f) {
            color = TileColors::applyHeightShading((state.height[i] - minHeight) / heightRange);
        }
        // Перегной — под травой: на клетке может быть и то, и другое
        // (семя охотно прорастает там, где перегной возвращает минералы в
        // почву), и тогда сверху видна именно трава.
        if (layers.plants && state.humus[i] > 0) {
            color = TileColors::humus(color, state.humus[i]);
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
        // Трава — только пока её не рисуют фигурами. Иначе клетка красилась
        // бы дважды: раз текселем и раз рисунком поверх него (см.
        // Layers::grassSprites).
        if (layers.plants && !layers.grassSprites && state.plantSpeciesAt[i] >= 0) {
            color = TileColors::plant(color, state.plantSpeciesAt[i], state.plantGrowth[i]);
        }
        // Куст — тем же правилом, что и трава, и по той же причине. В
        // отличие от дерева он помещается в клетку целиком, поэтому и в
        // текселе его можно выразить, — а дерево из клетки торчит вверх, и
        // под ним в текстуре всегда голая земля. Ягоды гаснут вместе с
        // кустом: их рисует тот же лист (PlantSprites::berries).
        if (layers.plants && !layers.bushSprites && !state.bushSpeciesAt.empty() &&
            state.bushSpeciesAt[i] >= 0) {
            color = TileColors::bush(color, state.bushSpeciesAt[i], state.plantGrowth[i]);
            if (!state.berries.empty() && state.berries[i] > 0) {
                color = TileColors::berries(color, state.berries[i]);
            }
        }
        // Подстилка — под кучей и под навесом: она самая нижняя из
        // сделанного руками.
        if (layers.goblins && !state.bedding.empty() && state.bedding[i] > 0.0f) {
            color = TileColors::bedding(color, state.bedding[i]);
        }
        // Куча — поверх всего, что лежит на земле: она и лежит поверх.
        // Гаснет вместе с гоблинами, а не с травой: её принесли руки, и без
        // тех, кто носит, её бы не было.
        if (layers.goblins && !state.store.empty() && state.store[i] > 0) {
            color = TileColors::store(color, state.store[i]);
        }
        // Навес — поверх всего: он над головой и закрывает собой и кучу, и
        // подстилку. Площадка — там, где ещё ничего не построено.
        if (layers.goblins && !state.canopy.empty() && state.canopy[i] > 0.0f) {
            color = TileColors::canopy(color, state.canopy[i]);
        }
        if (layers.goblins && !state.site.empty() && state.site[i] > 0) {
            color = TileColors::site(color);
        }
        // Вода — не полупрозрачный слой поверх, а готовый цвет тайла
        // (TileColors::water непрозрачен), поэтому просто заменяет
        // предыдущий.
        if (layers.moisture && state.waterDepth[i] > 0.0f) {
            color = TileColors::water(state.waterDepth[i]);
        }
        // Дерева здесь нет намеренно. Тексель карты — это клетка, а дерево
        // клеткой не выражается: оно из неё торчит вверх, на клетку выше
        // (см. WorldScreen, где оно и рисуется фигурой поверх карты). В
        // текстуре под ним остаётся голая земля — та, в которую оно
        // воткнуто.
        return color;
    }
}

void Cache::rebuildPixels(const WorldState& state, const Layers& layers) {
    const std::size_t cellCount = static_cast<std::size_t>(state.areaWidth) * state.areaHeight;
    pixels_.assign(cellCount, Color{0, 0, 0, 255});
    for (std::size_t i = 0; i < cellCount; ++i) {
        pixels_[i] = colorAt(state, layers, i);
    }
}

const Texture2D& Cache::texture(const WorldState& state, const Layers& layers) {
    const bool sizeMatches = loaded_ && builtWidth_ == state.areaWidth && builtHeight_ == state.areaHeight;
    if (sizeMatches && builtVersion_ == state.mapVersion && builtLayers_ == layers) {
        return texture_;
    }

    const std::size_t cellCount = static_cast<std::size_t>(state.areaWidth) * state.areaHeight;
    // Пересчитать только изменившиеся клетки можно ровно тогда, когда кэш
    // отстал РОВНО НА ОДИН снимок: список в снимке описывает изменения
    // относительно предыдущего снимка и только их. Отстал на два — какие
    // клетки изменились в пропущенном, знать неоткуда, и остаётся честно
    // пересчитать всё.
    //
    // Пустой список при новой версии — это world_init: мир приехал целиком,
    // менялось всё сразу, и списка у него нет вовсе.
    //
    // Порог в четверть мира — не про скорость, а про смысл: дельта, тронувшая
    // столько, — это уже не дельта. Замерено (tools/watch_map_rebuilds.py),
    // что живая дельта трогает 5.5% клеток мира 400x400 и 8% мира 200x130.
    const bool patchable = sizeMatches && builtLayers_ == layers &&
                           state.mapVersion == builtVersion_ + 1 && !state.changedCells.empty() &&
                           state.changedCells.size() * 4 < cellCount;
    if (patchable) {
        for (const std::int32_t cell : state.changedCells) {
            const std::size_t i = static_cast<std::size_t>(cell);
            // Номер приходит из сети: за границы массива по нему писать
            // нельзя, каким бы битым сообщение ни оказалось.
            if (cell >= 0 && i < pixels_.size()) {
                pixels_[i] = colorAt(state, layers, i);
            }
        }
    } else {
        rebuildPixels(state, layers);
    }
    builtVersion_ = state.mapVersion;
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

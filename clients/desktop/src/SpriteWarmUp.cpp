#include "SpriteWarmUp.hpp"

#include "BuildSprites.hpp"
#include "DeerSprites.hpp"
#include "GoblinSprites.hpp"
#include "PlantSprites.hpp"
#include "SpriteAtlas.hpp"
#include "TreeSprites.hpp"
#include "WaterSprites.hpp"
#include "WolfSprites.hpp"

namespace SpriteWarmUp {

void bakeAll() {
    // Спрашиваем годность — этого довольно: годность и печёт (см. baked() в
    // любом из модулей). Ответ никому не нужен, нужен побочный ход.
    //
    // Порядок — тот, в котором они рисуются на карте: земля, растения,
    // постройки, звери. Ему всё равно, каким быть, но пусть будет тем же, в
    // каком клиент о них думает.
    using D = SpriteAtlas::Detail;
    for (const D detail : {D::Coarse, D::Fine}) {
        (void)WaterSprites::ready(detail);
        (void)PlantSprites::grassReady(detail);
        (void)PlantSprites::bushReady(detail);
        (void)TreeSprites::ready(detail);
        (void)WolfSprites::ready(detail);
        (void)DeerSprites::ready(detail);
        (void)GoblinSprites::ready(detail);
    }
    // У постройки крупного рисунка нет вовсе, поэтому и подробности у неё нет.
    (void)BuildSprites::ready();

    // Заливка на видеокарту отложена до первого обращения к текстуре
    // (SpriteAtlas::Page::texture), и вот оно: один раз, готовым листом,
    // вместо одной заливки на каждый кадр, в котором на лист что-то легло.
    (void)SpriteAtlas::page().texture();
}

} // namespace SpriteWarmUp

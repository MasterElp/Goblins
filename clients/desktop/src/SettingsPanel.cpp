#define RAYGUI_IMPLEMENTATION
#include "SettingsPanel.hpp"

#include <raygui.h>

namespace {

// Строка стала заметно выше (было 36px, слайдер — 15px): на 4K/зумленных
// экранах прежние слайдеры было тяжело зацепить мышью.
constexpr float kRowHeight = 44.0f;
constexpr float kSectionGap = 12.0f;

// Единственное место, где перечислены все строки панели — и для отрисовки
// (DrawOps), и для расчёта высоты контента (MeasureOps, см. ниже).
// Раньше высота считалась через константы kParamRows/kSectionHeaders,
// которые нужно было руками держать в синхроне со списком строк — при
// любой правке они расходились и скролл либо обрезал последние
// параметры, либо оставлял пустой хвост. Теперь высота — это ровно то,
// что реально нарисовано: добавить параметр можно, добавив вызов сюда, и
// больше нигде.
template <typename Ops>
void layoutParams(Ops& ops, goblins::RegenerationRequest& edited) {
    ops.section("Seeds");
    ops.unsignedSeedRow("Terrain seed", edited.terrain_seed);
    ops.unsignedSeedRow("Boulder seed", edited.boulder_seed);
    ops.unsignedSeedRow("Plant seed", edited.plant_seed);

    ops.section("Boulders");
    ops.intRow("Boulder count", edited.boulder_count, 0, 300);

    ops.section("Grass");
    // Число видов травы — 3..12 (ядро всё равно обрежет значение к этим
    // границам): меньше трёх видов не даёт конкуренции, больше
    // двенадцати — виды перестают отличаться друг от друга при одном и
    // том же бюджете преимуществ.
    ops.intRow("Species", edited.plants.grass_species, 3, 12);
    // Стартовая заселённость, а не итоговая: дальше трава расселяется
    // сама и занимает всё, что ей подходит.
    ops.floatRow("Initial coverage", edited.plants.grass_coverage, 0.0f, 0.4f, 3);
    // Свойства мира (06_GameLoop.md, п.1a), как и порог минералов выше:
    // выбираются при генерации, во время симуляции не меняются.
    // Мутация — доля вложения черты, а не доля значения гена (у всех
    // черт вложение живёт в одном диапазоне, поэтому настройка одна на
    // весь геном).
    ops.floatRow("Mutation rate", edited.plants.mutation_rate, 0.0f, 0.3f, 3);
    // Сколько крупиц минералов перегной возвращает в почву за тик.
    ops.floatRow("Humus decay (per tick)", edited.plants.humus_decay_rate, 0.001f, 0.2f, 3);

    ops.section("Noise frequency (smaller = bigger shapes)");
    ops.floatRow("Height", edited.terrain.height_noise_frequency, 0.002f, 0.2f);
    ops.floatRow("Rockiness", edited.terrain.rock_noise_frequency, 0.002f, 0.2f);
    ops.floatRow("Compaction", edited.terrain.compaction_noise_frequency, 0.002f, 0.2f);
    ops.floatRow("Moisture", edited.terrain.moisture_noise_frequency, 0.002f, 0.2f);
    ops.floatRow("Minerals", edited.terrain.minerals_noise_frequency, 0.002f, 0.2f);

    ops.section("Fractal noise shape");
    ops.intRow("Octaves", edited.terrain.noise_octaves, 1, 8);
    ops.floatRow("Lacunarity", edited.terrain.noise_lacunarity, 1.0f, 4.0f);
    ops.floatRow("Gain", edited.terrain.noise_gain, 0.1f, 0.9f);

    ops.section("Height bumps (keeps river off rock/packed ground)");
    ops.floatRow("Rock bump", edited.terrain.rock_height_bump, 0.0f, 1.0f);
    ops.floatRow("Compaction bump", edited.terrain.compaction_height_bump, 0.0f, 1.0f);

    ops.section("River");
    ops.intRow("Count", edited.terrain.river_count, 0, 20);
    ops.floatRow("Width (tiles)", edited.terrain.river_width, 1.0f, 12.0f);
    ops.floatRow("Sinuosity", edited.terrain.river_sinuosity, 0.0f, 1.0f);
    ops.floatRow("Depth", edited.terrain.river_depth, 0.2f, 5.0f);
    ops.floatRow("Max flow speed", edited.terrain.river_max_flow_speed, 0.1f, 10.0f);
    // Минимальное падение дна на тайл пути. На нуле русло просто
    // повторяет рельеф со всеми буграми — вода стоит в локальных ямах
    // вместо того, чтобы течь по руслу.
    ops.floatRow("Bed slope (per tile)", edited.terrain.river_bed_slope, 0.0f, 0.05f);

    ops.section("Ponds");
    ops.floatRow("Min depth", edited.terrain.min_pond_depth, 0.0f, 0.5f);
    ops.intRow("Min size (tiles)", edited.terrain.min_pond_size, 1, 200);
    ops.intRow("Max size (0=none)", edited.terrain.max_pond_size, 0, 2000);
    ops.floatRow("Depth scale", edited.terrain.pond_depth_scale, 0.5f, 10.0f);

    ops.section("Moisture");
    ops.floatRow("Falloff (tiles)", edited.terrain.moisture_falloff, 1.0f, 40.0f);
    ops.floatRow("Water boost", edited.terrain.water_moisture_boost, 0.0f, 1.0f);
    ops.floatRow("Rock reduction", edited.terrain.rock_moisture_reduction, 0.0f, 1.0f);

    ops.section("Minerals");
    ops.floatRow("Average", edited.terrain.minerals_average, 0.0f, 50.0f);
    ops.intRow("River value", edited.terrain.river_minerals, 0, 50);
    // Свойство мира (06_GameLoop.md, п.1a), не текущий параметр
    // симуляции: выбирается здесь, при генерации, и во время самой
    // симуляции HydrologySystem его больше не меняет.
    ops.floatRow("Moisture threshold", edited.terrain.mineral_moisture_threshold, 0.0f, 1.0f);

    ops.section("Water flow");
    // Свойство мира (06_GameLoop.md, п.1a): доля разницы уровней
    // поверхности, перетекающая к самому низкому соседу за тик — больше
    // значение, быстрее вода растекается/заполняет впадины (и быстрее
    // приток от источника расходится по руслу, а не скапливается рядом с
    // истоком).
    ops.floatRow("Flow rate", edited.terrain.water_flow_rate, 0.0f, 1.0f);
    // Добавка к скорости за уклон: чем круче склон, тем быстрее по нему
    // течёт. С одной лишь Flow rate скорость всюду одинаковая, и чтобы
    // протолкнуть приток дальше по руслу, воде приходится копить у
    // истока большой стоячий перепад — тот самый "конус" у источника.
    ops.floatRow("Slope boost", edited.terrain.water_slope_boost, 0.0f, 20.0f);

    ops.section("Erosion");
    // Доля перенесённой воды, превращающаяся в вымытую породу. Порода не
    // исчезает: ровно столько же оседает там, куда пришла вода.
    // Каменистая и утрамбованная почва размывается заметно медленнее.
    ops.floatRow("Erosion rate", edited.terrain.soil_erosion_rate, 0.0f, 0.5f);
    // Потолок выемки относительно соседа — без него клетка под
    // источником размывается без остановки в бездонную яму.
    ops.floatRow("Max scour depth", edited.terrain.max_erosion_depth, 0.0f, 3.0f);
    // Доля основного размыва, заодно достающаяся соседям клетки-истока —
    // берег осыпается вместе с руслом, а не остаётся резкой стенкой.
    ops.floatRow("Erosion spread", edited.terrain.erosion_spread_rate, 0.0f, 0.3f);

    ops.section("Map edges");
    // Доля глубины, стекающая "за карту" у тайлов на самой границе —
    // без этого края ведут себя как стенки и вода копится у кромки.
    ops.floatRow("Edge drain rate", edited.terrain.edge_drain_rate, 0.0f, 0.2f);

    ops.section("Water sources");
    ops.intRow("Extra springs", edited.terrain.water_source_count, 0, 20);
    // Оба — свойства мира, как порог минералов выше. precision=6 у
    // Evaporation rate — иначе такое маленькое значение читалось бы как
    // "0.0000". Source strength — АБСОЛЮТНЫЙ приток (глубина за тик), не
    // множитель Evaporation rate (было так раньше — и при маленьком
    // испарении даже огромный множитель давал ничтожный приток, источник
    // не мог угнаться за собственным оттоком через течение).
    ops.floatRow("Evaporation rate", edited.terrain.water_evaporation_rate, 0.0f, 0.001f, 6);
    ops.floatRow("Source strength", edited.terrain.water_source_strength, 0.0f, 2.0f);
}

// Только считает высоту, ничего не рисует — используется до
// GuiScrollPanel, чтобы задать ей настоящую высоту контента.
struct MeasureOps {
    float height = 0.0f;

    void section(const char*) { height += kRowHeight + kSectionGap; }
    void floatRow(const char*, float&, float, float, int precision = 4) {
        (void)precision;
        height += kRowHeight;
    }
    void intRow(const char*, int&, int, int) { height += kRowHeight; }
    void unsignedSeedRow(const char*, unsigned&) { height += kRowHeight; }
};

// Рисует те же строки, что и MeasureOps считает — порядок и состав вызовов
// в layoutParams общий для обоих, поэтому измеренная высота никогда не
// расходится с нарисованной.
struct DrawOps {
    float x;
    float y;
    float rowWidth;

    void section(const char* title) {
        GuiLabel(Rectangle{x, y, rowWidth, kRowHeight}, title);
        y += kRowHeight + kSectionGap;
    }

    // precision — знаков после запятой в подписи; по умолчанию 4 хватает
    // почти всем параметрам, но у совсем маленьких (например,
    // water_evaporation_rate, доли тысячной) %.4f показал бы "0.0000" —
    // неотличимо от нуля.
    void floatRow(const char* label, float& value, float lo, float hi, int precision = 4) {
        GuiLabel(Rectangle{x, y, rowWidth, 18}, TextFormat("%s: %.*f", label, precision, value));
        GuiSliderBar(Rectangle{x, y + 20, rowWidth, kRowHeight - 24}, nullptr, nullptr, &value, lo, hi);
        y += kRowHeight;
    }

    void intRow(const char* label, int& value, int lo, int hi) {
        float f = static_cast<float>(value);
        GuiLabel(Rectangle{x, y, rowWidth, 18}, TextFormat("%s: %d", label, value));
        GuiSliderBar(Rectangle{x, y + 20, rowWidth, kRowHeight - 24}, nullptr, nullptr, &f, static_cast<float>(lo),
                     static_cast<float>(hi));
        value = static_cast<int>(f + 0.5f);
        y += kRowHeight;
    }

    void unsignedSeedRow(const char* label, unsigned& value) {
        float f = static_cast<float>(value);
        GuiLabel(Rectangle{x, y, rowWidth - 70, 18}, TextFormat("%s: %u", label, value));
        bool randomPressed = GuiButton(Rectangle{x + rowWidth - 60, y - 2, 60, 22}, "Random");
        GuiSliderBar(Rectangle{x, y + 20, rowWidth, kRowHeight - 24}, nullptr, nullptr, &f, 0.0f, 999999.0f);
        if (randomPressed) {
            value = static_cast<unsigned>(GetRandomValue(0, 999999));
        } else {
            value = static_cast<unsigned>(f + 0.5f);
        }
        y += kRowHeight;
    }
};

} // namespace

void SettingsPanel::loadFrom(const goblins::RegenerationRequest& current, bool force) {
    if (loaded_ && !force) {
        return;
    }
    edited_ = current;
    loaded_ = true;
}

bool SettingsPanel::draw(Rectangle bounds, goblins::RegenerationRequest& outRequest, bool& outSaveRequested) {
    // Кнопки (Regenerate/Save values/Reset) — вне прокрутки, в
    // отдельной полосе внизу панели, всегда видимы: раньше их приходилось
    // домётывать до конца длинного списка параметров, чтобы нажать.
    constexpr float kFooterHeight = 110.0f;
    const Rectangle scrollBounds{bounds.x, bounds.y, bounds.width, bounds.height - kFooterHeight};
    const Rectangle footerBounds{bounds.x, bounds.y + bounds.height - kFooterHeight, bounds.width, kFooterHeight};

    // Immediate-mode: сначала измеряем реальную высоту контента (тем же
    // проходом по layoutParams, что и отрисовка), чтобы задать
    // GuiScrollPanel настоящую высоту — иначе скролл не появится или
    // обрежет строки.
    MeasureOps measure;
    layoutParams(measure, edited_);
    const float contentHeight = measure.height + kSectionGap;

    static Rectangle view{};
    const Rectangle content{0, 0, scrollBounds.width - 18, contentHeight};

    GuiScrollPanel(scrollBounds, "Generation settings", content, &scroll_, &view);

    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y), static_cast<int>(view.width),
                      static_cast<int>(view.height));

    DrawOps draw{scrollBounds.x + scroll_.x + 8, scrollBounds.y + scroll_.y, scrollBounds.width - 34};
    layoutParams(draw, edited_);

    EndScissorMode();

    const float footerX = footerBounds.x + 8;
    const float footerRowWidth = footerBounds.width - 16;
    float footerY = footerBounds.y + 8;

    bool regenerate = false;
    if (GuiButton(Rectangle{footerX, footerY, footerRowWidth, 30}, "Regenerate")) {
        regenerate = true;
    }
    footerY += 30 + 6;
    outSaveRequested = GuiButton(Rectangle{footerX, footerY, footerRowWidth, 28}, "Save values");
    footerY += 28 + 6;
    if (GuiButton(Rectangle{footerX, footerY, footerRowWidth, 26}, "Reset to current server state")) {
        loaded_ = false; // следующий loadFrom(..., force=false) перезапишет edited_
    }

    if (regenerate) {
        outRequest = edited_;
        return true;
    }
    return false;
}

#define RAYGUI_IMPLEMENTATION
#include "SettingsPanel.hpp"

#include <raygui.h>

namespace {

// Строка стала заметно выше (было 36px, слайдер — 15px): на 4K/зумленных
// экранах прежние слайдеры было тяжело зацепить мышью.
constexpr float kRowHeight = 44.0f;
constexpr float kSectionGap = 12.0f;
// Заголовок полосы (GENERATION / WORLD PROPERTIES) — выше строки-секции и
// с чертой под ним: это деление важнее, чем деление на секции внутри.
constexpr float kGroupHeight = 40.0f;

// Единственное место, где перечислены все строки панели — и для отрисовки
// (DrawOps), и для расчёта высоты контента (MeasureOps, см. ниже).
// Раньше высота считалась через константы kParamRows/kSectionHeaders,
// которые нужно было руками держать в синхроне со списком строк — при
// любой правке они расходились и скролл либо обрезал последние
// параметры, либо оставлял пустой хвост. Теперь высота — это ровно то,
// что реально нарисовано: добавить параметр можно, добавив вызов сюда, и
// больше нигде.
// Порядок здесь — не косметика, а главное деление этих параметров: одни
// работают ОДИН РАЗ, в момент "Regenerate" (форма рельефа, где лечь рекам,
// сколько насыпать булыжников), другие генерация только ВЫБИРАЕТ, а
// работают они потом каждый тик (WorldPropertiesComponent — течение,
// эрозия, мутация; 06_GameLoop.md, п.1a). Крутить их вслепую вперемешку
// бессмысленно: у первых видимый результат появляется сразу после
// регенерации и дальше не меняется, у вторых — только на запущенной
// симуляции и тем позже, чем меньше значение. Внутри каждой полосы —
// группировка по тому, чего параметр касается.
template <typename Ops>
void layoutParams(Ops& ops, goblins::RegenerationRequest& edited) {
    ops.group("GENERATION -- applied on Regenerate");

    ops.section("Seed");
    // Один seed на весь мир — не три отдельных (террейн/булыжники/трава):
    // внутри генерации он расходится по стадиям сам (server/main.cpp), а
    // крутить снаружи три числа ради "получить другой мир" только путало.
    ops.unsignedSeedRow("World seed", edited.seed);

    ops.section("Terrain shape");
    // Масштаб рельефа: меньше — крупнее формы. Частоты остальных слоёв
    // (каменистость, утрамбованность, минералы) — кратные от неё, ядро
    // считает их само.
    ops.floatRow("Scale (smaller = bigger shapes)", edited.terrain.noise_frequency, 0.002f, 0.2f);
    ops.intRow("Octaves (detail layers)", edited.terrain.noise_octaves, 1, 8);
    // Насколько твёрдая земля (камень + утрамбовка) поднимает рельеф —
    // вода такие участки огибает, без ручных исключений "здесь реки быть
    // не может".
    ops.floatRow("Hard ground bump", edited.terrain.hardness_height_bump, 0.0f, 1.5f);

    ops.section("Rivers");
    ops.intRow("Count", edited.terrain.river_count, 0, 20);
    ops.floatRow("Width (tiles)", edited.terrain.river_width, 1.0f, 12.0f);
    ops.floatRow("Sinuosity", edited.terrain.river_sinuosity, 0.0f, 1.0f);
    ops.floatRow("Depth", edited.terrain.river_depth, 0.2f, 5.0f);

    ops.section("Ponds");
    // Средняя глубина воды пруда — в тех же единицах, что и глубина реки.
    // Где именно быть пруду и какого размера, решает рельеф (Priority-
    // Flood), а не отдельные фильтры по размеру.
    ops.floatRow("Depth", edited.terrain.pond_depth, 0.0f, 5.0f);

    ops.section("Springs");
    // Сколько "родников" в случайных точках карты — плюс по одному
    // автоматически на исток каждой реки. Сколько их — решается при
    // генерации; насколько они сильные — уже свойство мира, ниже.
    ops.intRow("Extra springs", edited.terrain.water_source_count, 0, 20);

    ops.section("Soil & boulders");
    // Среднее по карте; дальше минералы разносит течение (HydrologySystem)
    // и возвращает в почву перегной.
    ops.floatRow("Minerals average", edited.terrain.minerals_average, 0.0f, 50.0f);
    ops.intRow("Boulder count", edited.boulder_count, 0, 300);

    ops.section("Grass seeding");
    // Число видов травы — 3..12 (ядро всё равно обрежет значение к этим
    // границам): меньше трёх видов не даёт конкуренции, больше
    // двенадцати — виды перестают отличаться друг от друга при одном и
    // том же бюджете преимуществ.
    ops.intRow("Species", edited.plants.grass_species, 3, 12);
    // Стартовая заселённость, а не итоговая: дальше трава расселяется
    // сама и занимает всё, что ей подходит.
    ops.floatRow("Initial coverage", edited.plants.grass_coverage, 0.0f, 0.4f, 3);

    ops.group("WORLD PROPERTIES -- chosen here, read every tick");

    ops.section("Water flow");
    // АБСОЛЮТНЫЙ приток (глубина за тик), не множитель испарения: источник
    // должен перекрывать и своё испарение, и то, что течение уносит
    // соседям, при любых настройках.
    ops.floatRow("Source strength", edited.terrain.water_source_strength, 0.0f, 2.0f);
    // Доля разницы уровней поверхности, перетекающая к самому низкому
    // соседу за тик на ровном месте (на склоне ядро добавляет уклон).
    ops.floatRow("Flow rate", edited.terrain.water_flow_rate, 0.0f, 1.0f);

    ops.section("Erosion");
    // Доля перенесённой воды, превращающаяся в вымытую породу. Порода не
    // исчезает: ровно столько же оседает там, куда пришла вода.
    ops.floatRow("Erosion rate", edited.terrain.soil_erosion_rate, 0.0f, 0.5f);
    // Потолок выемки относительно соседа — без него клетка под источником
    // размывается без остановки в бездонную яму.
    ops.floatRow("Max scour depth", edited.terrain.max_erosion_depth, 0.0f, 3.0f);

    ops.section("Plant life");
    // Мутация — доля вложения черты, а не доля значения гена (у всех черт
    // вложение живёт в одном диапазоне, поэтому настройка одна на весь
    // геном).
    ops.floatRow("Mutation rate", edited.plants.mutation_rate, 0.0f, 0.3f, 3);
    // Сколько крупиц минералов перегной возвращает в почву за тик.
    ops.floatRow("Humus decay (per tick)", edited.plants.humus_decay_rate, 0.001f, 0.2f, 3);
}

// Только считает высоту, ничего не рисует — используется до
// GuiScrollPanel, чтобы задать ей настоящую высоту контента.
struct MeasureOps {
    float height = 0.0f;

    void group(const char*) { height += kGroupHeight + kSectionGap; }
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

    void group(const char* title) {
        DrawText(title, static_cast<int>(x), static_cast<int>(y + 14), 16, Color{235, 200, 110, 255});
        DrawLine(static_cast<int>(x), static_cast<int>(y + kGroupHeight - 6), static_cast<int>(x + rowWidth),
                 static_cast<int>(y + kGroupHeight - 6), Color{120, 105, 60, 255});
        y += kGroupHeight + kSectionGap;
    }

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

    // Заполняем всегда, а не только по "Regenerate": по "Save values"
    // на диск должно уйти ровно то, что сейчас набрано на панели.
    outRequest = edited_;
    return regenerate;
}

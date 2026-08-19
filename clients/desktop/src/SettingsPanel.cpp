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
// Размер, совпадающий с готовым выбором. Пресеты — не украшение: 200x200 и
// 400x400 это те два размера, ради которых размер вообще понадобилось
// выбирать (мир "побольше" и "совсем большой"), а всё остальное набирается
// вручную.
bool isPresetArea(int width, int height) {
    return (width == 200 && height == 200) || (width == 400 && height == 400);
}

template <typename Ops>
void layoutParams(Ops& ops, goblins::RegenerationRequest& edited, bool& customArea) {
    ops.group("GENERATION -- applied on Regenerate");

    ops.section("Map size");
    // Размер Области — первым: он решает, сколько места вообще будет у
    // рек, лугов и стада, и менять его после подбора всего остального
    // значит подбирать всё заново.
    ops.areaRow(edited.area_width, edited.area_height, customArea);

    ops.section("Seed");
    // Один seed на весь мир — не три отдельных (террейн/булыжники/трава):
    // внутри генерации он расходится по стадиям сам (server/main.cpp), а
    // крутить снаружи три числа ради "получить другой мир" только путало.
    ops.unsignedSeedRow("World seed", edited.seed);

    ops.section("Terrain shape");
    // Размер узора в тайлах: больше — крупнее формы. Масштабы остальных
    // слоёв (каменистость, минералы) — доли от него, ядро считает их само.
    ops.intRow("Feature size (tiles)", edited.terrain.feature_size, 5, 500);
    ops.intRow("Octaves (detail layers)", edited.terrain.noise_octaves, 1, 8);
    // Высота высочайшей вершины — в тех же единицах, что глубина воды.
    // Чем больше, тем круче спуск от истока к краю мира; при высоте
    // порядка глубины реки рельефа фактически нет и вода расползается по
    // плоскому.
    ops.intRow("Mountain height", edited.terrain.mountain_height, 1000, 60000);
    // Насколько высота делает почву твёрдой: горы и подножия —
    // преимущественно камень и слежавшийся грунт. 0 — каменистость сама по
    // себе, от шума; 1 — почти повторяет рельеф.
    ops.intRow("Mountains are rocky (per mille)", edited.terrain.mountain_hardness, 0, 1000);

    ops.section("Rivers");
    ops.intRow("Count", edited.terrain.river_count, 0, 20);
    ops.intRow("Width (tenths of a tile)", edited.terrain.river_width, 10, 120);
    ops.intRow("Sinuosity (per mille)", edited.terrain.river_sinuosity, 0, 1000);
    ops.intRow("Depth", edited.terrain.river_depth, 200, 5000);

    ops.section("Ponds");
    // Средняя глубина воды пруда — в тех же единицах, что и глубина реки.
    // Где именно быть пруду и какого размера, решает рельеф (Priority-
    // Flood), а не отдельные фильтры по размеру.
    ops.intRow("Depth", edited.terrain.pond_depth, 0, 5000);

    ops.section("Springs");
    // Сколько "родников" в случайных точках карты — плюс ровно по одному
    // автоматически на исток каждой реки. Именно это число и решает,
    // зальёт ли мир водой: источник стоит постоянным столбом и отдаёт
    // примерно свою глубину за тик, сколько бы из него ни вытекло.
    ops.intRow("Extra springs", edited.terrain.water_source_count, 0, 20);

    ops.section("Soil & boulders");
    // Среднее по карте; дальше минералы разносит течение (HydrologySystem)
    // и возвращает в почву перегной.
    ops.intRow("Minerals average", edited.terrain.minerals_average, 0, 50);
    ops.intRow("Boulder count", edited.boulder_count, 0, 300);

    ops.section("Grass seeding");
    // Число видов травы — 3..12 (ядро всё равно обрежет значение к этим
    // границам): меньше трёх видов не даёт конкуренции, больше
    // двенадцати — виды перестают отличаться друг от друга при одном и
    // том же бюджете преимуществ.
    ops.intRow("Species", edited.plants.grass_species, 3, 12);
    // Стартовая заселённость, а не итоговая: дальше трава расселяется
    // сама и занимает всё, что ей подходит.
    ops.intRow("Initial coverage (per mille)", edited.plants.grass_coverage, 0, 400);

    ops.section("Herbivores");
    // Число видов — 1..8 (ядро обрежет к этим границам): один вид — вполне
    // осмысленный мир, а больше восьми при общем бюджете преимуществ
    // перестают отличаться друг от друга, да и поголовье каждого
    // становится слишком мелким, чтобы животные находили пару.
    ops.intRow("Species", edited.animals.herbivore_species, 1, 8);
    // Стартовое поголовье в штуках, а не долей карты: животных десятки.
    // Дальше оно живёт само — размножается, голодает, гибнет от зубов и
    // вымирает по законам AnimalSystem. Ноль — мир вовсе без травоядных.
    ops.intRow("Initial head count", edited.animals.herbivore_count, 0, 400);

    ops.section("Predators");
    // Хищников по умолчанию в десять с лишним раз меньше, чем добычи, и
    // это не украшение: они едят её быстрее, чем она успевает
    // расплодиться. Ноль — мир без хищников (трава и стадо будут жить как
    // жили).
    ops.intRow("Species", edited.animals.predator_species, 1, 6);
    ops.intRow("Initial head count", edited.animals.predator_count, 0, 100);

    ops.group("WORLD PROPERTIES -- chosen here, read every tick");

    ops.section("Water sources");
    // Глубина СТОЛБА воды источника, а не приток за тик: источник всегда
    // стоит на этой глубине, сколько бы из него ни вытекло. Чем выше столб,
    // тем выше его поверхность и тем дальше он способен протолкнуть воду по
    // руслу. Скорости течения как настройки больше нет вовсе — вода просто
    // встаёт на один уровень внутри лужи (см. README, "Гидрология").
    ops.intRow("Source column depth", edited.terrain.water_source_depth, 0, 10000);

    ops.section("Water balance");
    // Вторая половина баланса воды. Столько тысячных глубины КАЖДАЯ водная
    // клетка теряет за сто тиков; источники же отдают ровно столько,
    // сколько унесёт течение, поэтому именно испарение (вместе с провалом
    // за край мира) и решает, сколько воды карта в итоге держит. Слишком
    // слабое испарение = карта, залитая целиком.
    ops.intRow("Evaporation (per 100 ticks)", edited.terrain.water_evaporation_rate, 0, 500);
    // Дождь: раз во сколько тиков он начинается и какой глубины каждая
    // капля. Дождь идёт несколько тиков и роняет капли в случайные точки
    // (форма — закон, см. kRainDurationTicks/kRainDropsPerTick), поэтому
    // за раз намокает лишь малая часть карты. Ноль в интервале — дождей в
    // этом мире нет.
    ops.intRow("Rain every (ticks)", edited.terrain.rain_interval_ticks, 0, 2000);
    ops.intRow("Rain drop depth", edited.terrain.rain_amount, 0, 300);

    ops.section("Erosion");
    // Доля перенесённой воды, превращающаяся в вымытую породу. Порода не
    // исчезает: ровно столько же оседает там, куда пришла вода.
    ops.intRow("Erosion rate (per mille)", edited.terrain.soil_erosion_rate, 0, 500);

    ops.section("Minerals");
    // Отключает только перенос минералов течением/влагой (HydrologySystem),
    // не сами крупицы: их по-прежнему добавляет и забирает перегной.
    ops.boolRow("Spread by water", edited.terrain.minerals_spread_enabled);

    ops.section("Plant life");
    // Мутация — доля вложения черты, а не доля значения гена (у всех черт
    // вложение живёт в одном диапазоне, поэтому настройка одна на весь
    // геном).
    ops.intRow("Mutation rate (per mille)", edited.plants.mutation_rate, 0, 300);
    // Раз во сколько тиков перегной возвращает в почву одну крупицу.
    ops.intRow("Humus: ticks per grain", edited.plants.humus_decay_period, 1, 1000);

    ops.section("Animal life");
    // Та же мутация, что и у растений, но своя: наследование детёныша
    // считается по таблицам черт животных, и настраивать их вместе значило
    // бы связать два независимых мира одним ползунком. Одна на обе диеты —
    // это скорость наследственных изменений в мире, а не свойство диеты.
    ops.intRow("Mutation rate (per mille)", edited.animals.mutation_rate, 0, 300);
}

// Только считает высоту, ничего не рисует — используется до
// GuiScrollPanel, чтобы задать ей настоящую высоту контента.
struct MeasureOps {
    float height = 0.0f;

    void group(const char*) { height += kGroupHeight + kSectionGap; }
    void section(const char*) { height += kRowHeight + kSectionGap; }
    void intRow(const char*, int&, int, int) { height += kRowHeight; }
    void boolRow(const char*, bool&) { height += kRowHeight; }
    void unsignedSeedRow(const char*, unsigned&) { height += kRowHeight; }
    // Строка размера выше остальных на два ползунка, когда размер
    // произвольный. Решение "показывать ли их" одно и то же здесь и в
    // отрисовке — оно считается по тем же самым полям, поэтому измеренная
    // высота не разойдётся с нарисованной.
    void areaRow(int& width, int& height_, bool& customArea) {
        height += kRowHeight;
        if (customArea || !isPresetArea(width, height_)) {
            height += kRowHeight * 2;
        }
    }
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

    // Дробной строки (floatRow) здесь больше нет: в панели генерации не
    // осталось ни одного дробного параметра — мир целочислен, и настройки
    // у него целые (core/Scale.hpp). Вместе с ней ушла и подпись с
    // точностью в знаках после запятой, которую приходилось задавать
    // отдельно каждому "слишком мелкому" параметру вроде испарения, иначе
    // ползунок показывал бы "0.0000" — неотличимо от нуля.
    void intRow(const char* label, int& value, int lo, int hi) {
        float f = static_cast<float>(value);
        GuiLabel(Rectangle{x, y, rowWidth, 18}, TextFormat("%s: %d", label, value));
        GuiSliderBar(Rectangle{x, y + 20, rowWidth, kRowHeight - 24}, nullptr, nullptr, &f, static_cast<float>(lo),
                     static_cast<float>(hi));
        value = static_cast<int>(f + 0.5f);
        y += kRowHeight;
    }

    void boolRow(const char* label, bool& value) {
        GuiCheckBox(Rectangle{x, y + 2, 20, 20}, label, &value);
        y += kRowHeight;
    }

    // Готовые размеры кнопками, всё остальное — двумя ползунками под
    // ними. Кнопки, а не ползунок по всем значениям подряд: 200 и 400 —
    // те самые размеры, ради которых выбор и заводился, и попадать в них
    // мышью с точностью до тайла было бы издевательством.
    void areaRow(int& width, int& height_, bool& customArea) {
        GuiLabel(Rectangle{x, y, rowWidth, 18}, TextFormat("Area: %d x %d tiles", width, height_));

        const float buttonWidth = (rowWidth - 16) / 3.0f;
        const float buttonY = y + 20;
        const float buttonHeight = kRowHeight - 24;
        const bool custom = customArea || !isPresetArea(width, height_);
        if (GuiButton(Rectangle{x, buttonY, buttonWidth, buttonHeight}, "200 x 200")) {
            width = 200;
            height_ = 200;
            customArea = false;
        }
        if (GuiButton(Rectangle{x + buttonWidth + 8, buttonY, buttonWidth, buttonHeight}, "400 x 400")) {
            width = 400;
            height_ = 400;
            customArea = false;
        }
        if (GuiButton(Rectangle{x + (buttonWidth + 8) * 2, buttonY, buttonWidth, buttonHeight},
                      custom ? "Custom (below)" : "Custom")) {
            // Значения не трогаем: "Custom" — это не другой размер, а
            // разрешение набрать его руками, начиная с текущего.
            customArea = true;
        }
        y += kRowHeight;

        if (custom) {
            intRow("Width (tiles)", width, goblins::kMinAreaSide, goblins::kMaxAreaSide);
            intRow("Height (tiles)", height_, goblins::kMinAreaSide, goblins::kMaxAreaSide);
        }
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
    // Мир нестандартного размера открывает строку размера сразу с
    // ползунками: иначе панель показывала бы "200x200 / 400x400 / Custom"
    // и ни одну из кнопок нажатой, хотя размер у мира вполне конкретный.
    customArea_ = !isPresetArea(edited_.area_width, edited_.area_height);
    loaded_ = true;
}

bool SettingsPanel::draw(Rectangle bounds, goblins::RegenerationRequest& outRequest, bool& outSaveRequested,
                          bool worldGenerated) {
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
    layoutParams(measure, edited_, customArea_);
    const float contentHeight = measure.height + kSectionGap;

    static Rectangle view{};
    const Rectangle content{0, 0, scrollBounds.width - 18, contentHeight};

    GuiScrollPanel(scrollBounds, "Generation settings", content, &scroll_, &view);

    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y), static_cast<int>(view.width),
                      static_cast<int>(view.height));

    DrawOps draw{scrollBounds.x + scroll_.x + 8, scrollBounds.y + scroll_.y, scrollBounds.width - 34};
    layoutParams(draw, edited_, customArea_);

    EndScissorMode();

    const float footerX = footerBounds.x + 8;
    const float footerRowWidth = footerBounds.width - 16;
    float footerY = footerBounds.y + 8;

    // Пока мира нет (сервер только поднялся или игрок нажал "New world" и
    // ещё не создал мир), эта кнопка не "перегенерировать", а "создать":
    // перегенерировать в пустом мире нечего, и подпись об этом должна
    // говорить прямо.
    bool regenerate = false;
    if (GuiButton(Rectangle{footerX, footerY, footerRowWidth, 30},
                  worldGenerated ? "Regenerate" : "Create world")) {
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

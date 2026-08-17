#include "WorldScreen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <raygui.h>
#include <raylib.h>

#include "ConstantsOverlay.hpp"
#include "InfoPanel.hpp"
#include "MapTexture.hpp"
#include "PopulationGraph.hpp"
#include "TileColors.hpp"

namespace WorldScreen {

namespace {

constexpr int kHudHeight = 32;
// Нижний предел масштаба ниже, чем нужно для просто "отдалить": вписывание
// всей карты в окно (клавиша F) на большой Области упирается именно в
// него, и слишком высокий пол не дал бы увидеть мир целиком.
constexpr float kMinZoom = 0.1f;
constexpr float kMaxZoom = 4.0f;
constexpr float kZoomStep = 1.1f;
// Ширина правой панели. То же число, что было у панели генерации на
// отдельном экране, — под ним подобраны ширины ползунков, а карточка
// существа и графики в него вписались.
constexpr float kPanelWidth = 460.0f;
// Полоса подсказки по клавишам вдоль нижнего края.
constexpr float kHintHeight = 26.0f;

// Правая панель одна на три содержимого: карточка того, на что смотришь,
// параметры генерации и графики численности. Одно место, а не три угла
// экрана: смотрят в них по очереди, а места они просят одинаково много.
enum class Tab { Hidden, Info, Params, Graphs };

const char* tabName(Tab tab) {
    switch (tab) {
        case Tab::Info: return "info";
        case Tab::Params: return "params";
        case Tab::Graphs: return "graphs";
        case Tab::Hidden: break;
    }
    return "hidden";
}

Tab tabFromName(const std::string& name) {
    if (name == "info") return Tab::Info;
    if (name == "params") return Tab::Params;
    if (name == "graphs") return Tab::Graphs;
    return Tab::Hidden;
}

// Порядок перебора клавишей P: три вкладки, затем свёрнутая панель. Свёрнутое
// состояние — часть того же круга, а не отдельная клавиша: панель и так
// закрывает треть экрана, и убрать её должно быть можно тем же движением,
// которым её листают.
Tab nextTab(Tab tab) {
    switch (tab) {
        case Tab::Info: return Tab::Params;
        case Tab::Params: return Tab::Graphs;
        case Tab::Graphs: return Tab::Hidden;
        case Tab::Hidden: break;
    }
    return Tab::Info;
}

} // namespace

AppScreen draw(NetworkClient& network, goblins::ClientConfig& config, const std::string& configPath,
               SettingsPanel& panel) {
    // Персистентны между кадрами, пока это состояние активно (при
    // возврате в меню и обратно позиция прокрутки сохраняется — это
    // осознанное поведение, не забытый сброс).
    static float viewX = 0.0f;
    static float viewY = 0.0f;
    // Масштаб, слои и открытость панели — заводятся один раз из config
    // (значения с диска или умолчания ClientConfig), дальше живут как
    // обычные static-переменные экрана; при изменении пишутся обратно в
    // config и на диск (ниже, у каждого места, где меняются).
    static bool initialized = false;
    static float zoom = 1.0f;
    static bool showRockiness = true;
    static bool showCompaction = true;
    static bool showMoisture = true;
    static bool showMinerals = true;
    static bool showHeight = true;
    static bool showPlants = true;
    static bool showAnimals = true;
    static Tab tab = Tab::Hidden;
    // За кем следим: выбранное кликом живёт, пока не выберут другое, — в
    // этом и смысл слежения. Наведение мышью его не сбивает.
    static InfoPanel::Target selection{};
    // Последнее, о чём сказано серверу: он присылает подробности только по
    // одной цели (см. "watch" в протоколе), и слать один и тот же запрос
    // каждый кадр незачем. Хранится ровно то, что ушло в сообщении, а не
    // сама цель: у почвы и у "ничего не выбрано" запрос одинаковый ("none"),
    // и переезд курсора с одной пустой клетки на другую поводом для
    // сообщения быть не должен.
    static std::string sentWatchKind;
    static std::uint64_t sentWatchId = 0;
    static int sentWatchX = 0;
    static int sentWatchY = 0;
    static bool sentWatchValid = false;
    if (!initialized) {
        zoom = config.zoom;
        showRockiness = config.show_rockiness;
        showCompaction = config.show_compaction;
        showMoisture = config.show_moisture;
        showMinerals = config.show_minerals;
        showHeight = config.show_height;
        showPlants = config.show_plants;
        showAnimals = config.show_animals;
        tab = tabFromName(config.panel_tab);
        initialized = true;
    }
    static bool confirmingExit = false;
    // Диалог ввода имени при сохранении — открывается кнопкой "Save
    // world", буфер предзаполняется именем текущего мира (пусто, если
    // мир ещё не сохранён).
    static bool showSaveDialog = false;
    static char saveNameBuffer[64] = "";
    // Карта в текстуре — тоже состояние экрана: пересобирается только
    // когда пришло новое состояние мира или переключён слой.
    static MapTexture::Cache mapCache;
    // Вписать карту в окно по клавише F: сам пересчёт масштаба возможен
    // только когда известен размер Области, а он приходит с сервера —
    // поэтому нажатие запоминается здесь и отрабатывается ниже, после
    // получения снапшота.
    bool fitRequested = false;

    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();
    // Панель генерации занимает правый край во всю высоту, карта — всё
    // остальное. Верхняя полоса тянется только над картой, а не над
    // панелью: панель прокручивается целиком, и полоса поверх неё съедала
    // бы первую строку параметров.
    const bool panelOpen = tab != Tab::Hidden;
    const float panelX = static_cast<float>(screenW) - kPanelWidth;
    const int viewportW = panelOpen ? std::max(1, static_cast<int>(panelX)) : screenW;
    const int viewportH = screenH - kHudHeight;

    // Полоса вкладок — вровень с верхней полосой над картой: это один и тот
    // же ряд элементов управления, просто над разными половинами экрана.
    const Rectangle panelBounds{panelX, 0, kPanelWidth, static_cast<float>(screenH)};
    const Rectangle tabsBounds{panelX, 0, kPanelWidth, static_cast<float>(kHudHeight)};
    const Rectangle panelContent{panelX + 10.0f, static_cast<float>(kHudHeight) + 8.0f, kPanelWidth - 20.0f,
                                 static_cast<float>(screenH) - kHudHeight - 8.0f - kHintHeight};

    const Color backgroundColor{28, 28, 32, 255};
    const Color boulderColor{70, 66, 62, 255};
    const Color hudColor{18, 18, 20, 255};
    const Color textColor{230, 230, 230, 255};
    const Color mutedColor{150, 150, 156, 255};
    const Color cursorColor{255, 220, 90, 255};
    const Color pausedColor{220, 70, 70, 255};

    float tileSizeF = static_cast<float>(config.tile_size) * zoom;

    const float dt = GetFrameTime();
    // Прокрутка — в тайлах в секунду, поэтому от зума скорость не зависит:
    // на любом масштабе карта проезжает мимо одинаково. Не настройка —
    // темп прокрутки подбирается один раз на ощупь и от мира к миру не
    // меняется.
    constexpr float kScrollTilesPerSecond = 5.0f;
    const float scrollSpeedPx = kScrollTilesPerSecond * tileSizeF;

    const Vector2 mouse = GetMousePosition();
    // Колесо над правой панелью прокручивает её саму (GuiScrollPanel) и
    // читает точку на графике — но не меняет масштаб карты: одно движение
    // колеса не должно делать сразу два несвязанных действия.
    const bool mouseOverMap = mouse.x < static_cast<float>(viewportW);

    // Пока открыт диалог подтверждения выхода или диалог сохранения, мир
    // под ним не должен реагировать на ввод (прокрутка/зум/слои/пауза) —
    // иначе клик по кнопке диалога совпадёт с движением камеры или сменой
    // слоя позади, а буквенные клавиши при вводе имени — с WASD/P/1-6.
    // Оверлей констант — по той же причине: он перекрывает экран целиком,
    // и слои под ним переключались бы вслепую.
    const bool inputBlocked = confirmingExit || showSaveDialog || ConstantsOverlay::visible();
    if (!inputBlocked) {
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) viewY -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) viewY += scrollSpeedPx * dt;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) viewX -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) viewX += scrollSpeedPx * dt;

        // Зум колёсиком мыши — к точке под курсором: мировая координата под
        // курсором остаётся на месте, чтобы приближение/отдаление не
        // "убегало" в сторону.
        const float wheel = mouseOverMap ? GetMouseWheelMove() : 0.0f;
        if (wheel != 0.0f) {
            const float worldX = mouse.x + viewX;
            const float worldY = mouse.y - kHudHeight + viewY;

            const float factor = wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep;
            zoom = std::clamp(zoom * factor, kMinZoom, kMaxZoom);
            tileSizeF = static_cast<float>(config.tile_size) * zoom;

            viewX = worldX * factor - mouse.x;
            viewY = worldY * factor - mouse.y;

            // Пишем на диск на каждый "тик" колеса, а не только когда
            // прокрутка остановилась: щёлкает колесо редко относительно
            // кадров (в отличие от WASD-прокрутки, идущей каждый кадр),
            // поэтому лишней нагрузки на диск это не создаёт.
            config.zoom = zoom;
            goblins::saveClientConfig(configPath, config);
        }

        // Вписать всю карту в окно — то, что раньше делал отдельный экран
        // генерации (он всегда показывал мир целиком). При подборе
        // параметров важно видеть форму рек и очертания прудов сразу, а не
        // проматывать карту после каждой регенерации.
        if (IsKeyPressed(KEY_F)) {
            fitRequested = true;
        }

        // Правая панель: одна клавиша листает её вкладки и сворачивает
        // саму панель (см. nextTab). Раньше на панель параметров и на
        // графики было по своей клавише, и обе панели могли занимать экран
        // одновременно, хотя смотрят в них по очереди.
        if (IsKeyPressed(KEY_P)) {
            tab = nextTab(tab);
            config.panel_tab = tabName(tab);
            goblins::saveClientConfig(configPath, config);
        }

        // Слои почвы — каждый можно исключить из смешения цвета тайла
        // (каменистость/плотность/влажность/минералы считаются нулевыми,
        // если слой выключен). Вода — тот же выключатель, что и влажность
        // (KEY_THREE): вода на карте — это и есть источник влажности,
        // раздельные флаги только путали бы (можно было увидеть воду при
        // погашенном слое влажности). Высота (KEY_FIVE) — не часть
        // смешения, а множитель яркости поверх готового цвета (см.
        // TileColors::applyHeightShading), поэтому переключается и
        // применяется отдельно от остальных четырёх.
        // Каждое переключение — редкое дискретное событие (не каждый
        // кадр, как WASD-прокрутка), поэтому сохраняем в config.json сразу,
        // без отдельной кнопки "Сохранить" — как и масштаб выше.
        if (IsKeyPressed(KEY_ONE)) {
            showRockiness = !showRockiness;
            config.show_rockiness = showRockiness;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_TWO)) {
            showCompaction = !showCompaction;
            config.show_compaction = showCompaction;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_THREE)) {
            showMoisture = !showMoisture;
            config.show_moisture = showMoisture;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_FOUR)) {
            showMinerals = !showMinerals;
            config.show_minerals = showMinerals;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_FIVE)) {
            showHeight = !showHeight;
            config.show_height = showHeight;
            goblins::saveClientConfig(configPath, config);
        }
        // Растения и перегной — один выключатель (KEY_SIX): перегной это
        // и есть след умершего растения, разделять их значило бы видеть
        // остатки при погашенном слое травы.
        if (IsKeyPressed(KEY_SIX)) {
            showPlants = !showPlants;
            config.show_plants = showPlants;
            goblins::saveClientConfig(configPath, config);
        }
        // Животные — отдельный выключатель, а не часть слоя травы: это не
        // слой почвы, а объекты поверх карты (как булыжники и источники),
        // и смотреть на луг без зверей — обычное дело. Падаль гаснет
        // вместе с ними: она их след, а не свойство почвы.
        if (IsKeyPressed(KEY_SEVEN)) {
            showAnimals = !showAnimals;
            config.show_animals = showAnimals;
            goblins::saveClientConfig(configPath, config);
        }

        // Пауза — не локальное состояние клиента, а запрос серверу (настоящая
        // пауза мира). Сам клиент своё "paused" не выставляет — ждёт
        // подтверждения через pause_state/world_delta, чтобы все
        // подключённые клиенты видели одно и то же состояние. Пробел, а не
        // буква: остановить и пустить время — самое частое действие на этом
        // экране, и рука находит его не глядя.
        if (IsKeyPressed(KEY_SPACE)) {
            network.sendTogglePause();
        }
    }

    // Состояние мира — разделяемым указателем на неизменяемый снимок:
    // копировать десяток массивов по числу тайлов каждый кадр незачем,
    // меняются они только с приходом сообщения сервера.
    const auto statePtr = network.snapshot();
    const WorldState& snapshot = *statePtr;

    if (snapshot.hasGeneration) {
        panel.loadFrom(snapshot.generation);
    }

    if (fitRequested && snapshot.areaWidth > 0 && snapshot.areaHeight > 0) {
        const float base = static_cast<float>(config.tile_size);
        const float fitZoom = std::min(static_cast<float>(viewportW) / (snapshot.areaWidth * base),
                                        static_cast<float>(viewportH) / (snapshot.areaHeight * base));
        zoom = std::clamp(fitZoom, kMinZoom, kMaxZoom);
        tileSizeF = base * zoom;
        // Карта после вписывания меньше окна (или равна ему) — прокрутка
        // обнуляется, иначе остался бы сдвиг от прежнего масштаба.
        viewX = 0.0f;
        viewY = 0.0f;
        config.zoom = zoom;
        goblins::saveClientConfig(configPath, config);
    }

    if (snapshot.areaWidth > 0) {
        const float maxX = std::max(0.0f, static_cast<float>(snapshot.areaWidth) * tileSizeF - viewportW);
        const float maxY = std::max(0.0f, static_cast<float>(snapshot.areaHeight) * tileSizeF - viewportH);
        viewX = std::clamp(viewX, 0.0f, maxX);
        viewY = std::clamp(viewY, 0.0f, maxY);
    } else {
        viewX = std::max(0.0f, viewX);
        viewY = std::max(0.0f, viewY);
    }

    const int tileSize = std::max(1, static_cast<int>(std::round(tileSizeF)));

    bool hasHoverTile = false;
    int hoverX = 0;
    int hoverY = 0;
    if (snapshot.areaWidth > 0 && mouseOverMap && mouse.x >= 0 && mouse.y >= kHudHeight &&
        mouse.y < kHudHeight + viewportH) {
        hoverX = static_cast<int>(std::floor((mouse.x + viewX) / tileSizeF));
        hoverY = static_cast<int>(std::floor((mouse.y - kHudHeight + viewY) / tileSizeF));
        hasHoverTile = hoverX >= 0 && hoverX < snapshot.areaWidth && hoverY >= 0 && hoverY < snapshot.areaHeight;
    }

    // Что стоит на клетке, в порядке перебора кликом: сперва существа
    // (сколько бы их ни было на одной клетке), потом растение, потом сама
    // почва. Клик по клетке со зверем должен показывать зверя, а не землю
    // под ним; повторный клик — того, кто стоит рядом с ним; и только
    // перебрав всех — траву и почву.
    const auto targetsAt = [&snapshot](int x, int y) {
        std::vector<InfoPanel::Target> targets;
        for (const auto& animal : snapshot.animals) {
            if (animal.x == x && animal.y == y) {
                targets.push_back(InfoPanel::Target{InfoPanel::Target::Kind::Animal, animal.id, x, y, true});
            }
        }
        const std::size_t index = static_cast<std::size_t>(y) * snapshot.areaWidth + x;
        if (index < snapshot.plantSpeciesAt.size() && snapshot.plantSpeciesAt[index] >= 0) {
            targets.push_back(InfoPanel::Target{InfoPanel::Target::Kind::Plant, 0, x, y, true});
        }
        targets.push_back(InfoPanel::Target{InfoPanel::Target::Kind::Soil, 0, x, y, true});
        return targets;
    };

    if (!inputBlocked && hasHoverTile && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const auto targets = targetsAt(hoverX, hoverY);
        // Клик по той же клетке сдвигает выбор на следующего в списке,
        // клик по новой — начинает список сначала.
        std::size_t next = 0;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (InfoPanel::sameTarget(targets[i], selection)) {
                next = (i + 1) % targets.size();
                break;
            }
        }
        selection = targets[next];
    }
    // Правая кнопка снимает выбор: иначе от закреплённого существа нельзя
    // было бы отвязаться, не выбрав вместо него что-то другое.
    if (!inputBlocked && mouseOverMap && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        selection = InfoPanel::Target{};
    }

    // Карточка показывает выбранное, а пока ничего не выбрано — то, что под
    // курсором: наведение отвечает на вопрос "а это кто?", клик — "покажи
    // мне его и дальше". Наведение при этом выбор не сбивает.
    InfoPanel::Target infoTarget = selection;
    if (infoTarget.kind == InfoPanel::Target::Kind::None && hasHoverTile) {
        infoTarget = targetsAt(hoverX, hoverY).front();
        infoTarget.pinned = false;
    }

    // Тело и геном приходят только по запросу (см. "watch" в протоколе):
    // геном каждого животного в каждой дельте весил бы больше, чем всё
    // остальное состояние мира. Поэтому серверу говорится ровно одна цель
    // и ровно тогда, когда она сменилась.
    {
        const char* watchKind = infoTarget.kind == InfoPanel::Target::Kind::Animal   ? "animal"
                                : infoTarget.kind == InfoPanel::Target::Kind::Plant ? "plant"
                                                                                     : "none";
        const std::uint64_t watchId =
            infoTarget.kind == InfoPanel::Target::Kind::Animal ? infoTarget.animalId : 0;
        const int watchX = infoTarget.kind == InfoPanel::Target::Kind::Plant ? infoTarget.x : 0;
        const int watchY = infoTarget.kind == InfoPanel::Target::Kind::Plant ? infoTarget.y : 0;
        if (!sentWatchValid || sentWatchKind != watchKind || sentWatchId != watchId || sentWatchX != watchX ||
            sentWatchY != watchY) {
            network.sendWatch(watchKind, watchId, watchX, watchY);
            sentWatchKind = watchKind;
            sentWatchId = watchId;
            sentWatchX = watchX;
            sentWatchY = watchY;
            sentWatchValid = true;
        }
    }

    ClearBackground(backgroundColor);

    if (!snapshot.connected || snapshot.areaWidth == 0) {
        const std::string waiting = "Connecting to " + config.host + ":" + std::to_string(config.port) + "...";
        DrawText(waiting.c_str(), 10, kHudHeight + 10, 20, textColor);
    } else {
        BeginScissorMode(0, kHudHeight, viewportW, viewportH);

        // Карта — одна текстура (тексель на тайл), пересобирается только
        // при новом состоянии мира или смене набора слоёв; за кадр это
        // один вызов отрисовки вместо тысяч прямоугольников, а отсечение
        // невидимой части делает сам ножничный режим.
        MapTexture::Layers layers;
        layers.rockiness = showRockiness;
        layers.compaction = showCompaction;
        layers.moisture = showMoisture;
        layers.minerals = showMinerals;
        layers.height = showHeight;
        layers.plants = showPlants;
        layers.animals = showAnimals;
        const Texture2D& mapTexture = mapCache.texture(snapshot, layers);
        DrawTexturePro(mapTexture,
                       Rectangle{0, 0, static_cast<float>(snapshot.areaWidth),
                                 static_cast<float>(snapshot.areaHeight)},
                       Rectangle{-viewX, kHudHeight - viewY, static_cast<float>(snapshot.areaWidth) * tileSizeF,
                                 static_cast<float>(snapshot.areaHeight) * tileSizeF},
                       Vector2{0, 0}, 0.0f, WHITE);

        for (const auto& boulder : snapshot.boulders) {
            const float screenX = static_cast<float>(boulder.first) * tileSizeF - viewX;
            const float screenY = static_cast<float>(boulder.second) * tileSizeF - viewY + kHudHeight;
            if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                screenY > viewportH + kHudHeight) {
                continue;
            }
            // На сильно отдалённой карте (вписанной в окно) тайл — считанные
            // пиксели, и отступ в 2px съел бы булыжник целиком.
            const int inset = tileSize > 5 ? 2 : 0;
            DrawRectangle(static_cast<int>(screenX) + inset, static_cast<int>(screenY) + inset,
                          std::max(1, tileSize - 2 * inset), std::max(1, tileSize - 2 * inset), boulderColor);
        }

        // Источники воды — тег без данных (истоки рек + "родники"),
        // отмечены кольцом поверх воды, как булыжники отмечены заливкой:
        // это не переключаемый слой почвы, а факт наличия/отсутствия.
        for (const auto& source : snapshot.waterSources) {
            const float screenX = static_cast<float>(source.first) * tileSizeF - viewX;
            const float screenY = static_cast<float>(source.second) * tileSizeF - viewY + kHudHeight;
            if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                screenY > viewportH + kHudHeight) {
                continue;
            }
            DrawCircleLines(static_cast<int>(screenX + tileSizeF / 2.0f), static_cast<int>(screenY + tileSizeF / 2.0f),
                             std::max(2.0f, tileSizeF * 0.35f), Color{130, 210, 255, 255});
        }

        // Животные — поверх карты, а не в её текстуре: они не свойство
        // тайла, их может быть несколько на одной клетке, и меняются они
        // каждый тик (пересобирать из-за них всю текстуру мира было бы
        // дороже всего остального вместе взятого). Самка — кружок, самец —
        // квадрат: пол виден сразу, а на сильно отдалённой карте и то, и
        // другое честно вырождается в точку. Хищник отличается цветом
        // (красная палитра против охристой) и тем, что рисуется крупнее:
        // на карте его должно быть видно первым.
        if (showAnimals) {
            for (const auto& animal : snapshot.animals) {
                const float screenX = static_cast<float>(animal.x) * tileSizeF - viewX;
                const float screenY = static_cast<float>(animal.y) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                const Color color = animal.predator ? TileColors::predatorSpecies(animal.species)
                                                     : TileColors::herbivoreSpecies(animal.species);
                // Размер значка — от развитости: детёныш мельче взрослого,
                // как и на самом деле; хищник крупнее добычи.
                const float scale = animal.predator ? 0.30f : 0.22f;
                const float radius = std::max(1.0f, tileSizeF * (scale + 0.16f * animal.growth));
                const float centerX = screenX + tileSizeF * 0.5f;
                const float centerY = screenY + tileSizeF * 0.5f;
                if (animal.sex == "male") {
                    DrawRectangle(static_cast<int>(centerX - radius), static_cast<int>(centerY - radius),
                                  std::max(1, static_cast<int>(radius * 2.0f)),
                                  std::max(1, static_cast<int>(radius * 2.0f)), color);
                } else {
                    DrawCircle(static_cast<int>(centerX), static_cast<int>(centerY), radius, color);
                }
                // Тёмная обводка — чтобы светлое животное не терялось на
                // светлой почве; только когда тайл достаточно крупный,
                // иначе она съест сам значок.
                if (tileSizeF >= 8.0f) {
                    DrawCircleLines(static_cast<int>(centerX), static_cast<int>(centerY), radius + 1.0f,
                                    Color{30, 24, 18, 200});
                }
                // Раненого видно: полоска поверх значка. Она появляется
                // только у пострадавшего — целому животному рисовать нечего.
                if (animal.health < 0.99f && tileSizeF >= 6.0f) {
                    const float barWidth = tileSizeF * 0.7f;
                    DrawRectangle(static_cast<int>(centerX - barWidth * 0.5f), static_cast<int>(screenY + 1.0f),
                                  std::max(1, static_cast<int>(barWidth * animal.health)), 2,
                                  Color{220, 60, 50, 230});
                }
            }
        }

        if (hasHoverTile) {
            const float screenX = static_cast<float>(hoverX) * tileSizeF - viewX;
            const float screenY = static_cast<float>(hoverY) * tileSizeF - viewY + kHudHeight;
            DrawRectangleLines(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize, cursorColor);
        }

        // Выбранное — поверх всего остального на карте. У существа метка
        // едет вместе с ним: его ищут по постоянному идентификатору, а не
        // по клетке, в которой по нему щёлкнули, — в этом и весь смысл
        // слежения. Пропало из списка (умерло, съедено) — метки нет, и
        // карточка скажет, что именно случилось.
        if (selection.kind != InfoPanel::Target::Kind::None) {
            int markX = selection.x;
            int markY = selection.y;
            bool hasMark = selection.kind != InfoPanel::Target::Kind::Animal;
            if (selection.kind == InfoPanel::Target::Kind::Animal) {
                for (const auto& animal : snapshot.animals) {
                    if (animal.id == selection.animalId) {
                        markX = animal.x;
                        markY = animal.y;
                        hasMark = true;
                        break;
                    }
                }
            }
            if (hasMark) {
                const float screenX = static_cast<float>(markX) * tileSizeF - viewX;
                const float screenY = static_cast<float>(markY) * tileSizeF - viewY + kHudHeight;
                const Color selectionColor{255, 255, 255, 235};
                if (selection.kind == InfoPanel::Target::Kind::Animal) {
                    // Кольцо, а не рамка: на клетке может стоять ещё
                    // десяток зверей, и рамка вокруг всей клетки не
                    // показала бы, за кем именно следят.
                    DrawCircleLines(static_cast<int>(screenX + tileSizeF * 0.5f),
                                     static_cast<int>(screenY + tileSizeF * 0.5f), std::max(4.0f, tileSizeF * 0.55f),
                                     selectionColor);
                } else {
                    DrawRectangleLinesEx(Rectangle{screenX, screenY, tileSizeF, tileSizeF}, 2.0f, selectionColor);
                }
            }
        }

        EndScissorMode();

        // Статус слоёв почвы — под верхней панелью, слева: включённые
        // слои белым, выключенные приглушённым серым, чтобы состояние
        // читалось с одного взгляда.
        const Color layerOnColor = textColor;
        const Color layerOffColor{110, 110, 110, 255};
        int layerX = 10;
        const int layerY = kHudHeight + 6;
        auto drawLayerLabel = [&](const char* label, bool enabled) {
            const Color c = enabled ? layerOnColor : layerOffColor;
            DrawText(label, layerX, layerY, 14, c);
            layerX += MeasureText(label, 14) + 14;
        };
        drawLayerLabel("[1] Rockiness", showRockiness);
        drawLayerLabel("[2] Compaction", showCompaction);
        drawLayerLabel("[3] Moisture+Water", showMoisture);
        drawLayerLabel("[4] Minerals", showMinerals);
        drawLayerLabel("[5] Height", showHeight);
        drawLayerLabel("[6] Grass+Humus", showPlants);
        drawLayerLabel("[7] Animals", showAnimals);

        // Виды травы: цвет, номер и текущая численность — сколько тайлов
        // занимает каждый вид прямо сейчас. Считается по тому же
        // плотному массиву, что и рисуется, поэтому легенда всегда
        // соответствует картинке. Это единственное место, где видно, как
        // виды делят мир между собой на протяжении симуляции.
        if (showPlants && !snapshot.plantSpecies.empty()) {
            std::vector<int> population(snapshot.plantSpecies.size(), 0);
            for (const int species : snapshot.plantSpeciesAt) {
                if (species >= 0 && static_cast<std::size_t>(species) < population.size()) {
                    ++population[static_cast<std::size_t>(species)];
                }
            }

            int legendX = 10;
            const int legendY = layerY + 20;
            for (std::size_t s = 0; s < population.size(); ++s) {
                DrawRectangle(legendX, legendY + 1, 10, 10, TileColors::plantSpecies(static_cast<int>(s)));
                const char* label = TextFormat("sp%zu %d", s, population[s]);
                DrawText(label, legendX + 14, legendY, 14, layerOnColor);
                legendX += 14 + MeasureText(label, 14) + 12;
            }
        }

        // Поголовье: сколько особей каждого вида и чем они сейчас заняты.
        // Желания в легенде не для красоты — по ним видно состояние мира
        // целиком: стадо, поголовно ищущее еду, означает объеденный луг, а
        // стадо, поголовно бегущее, — что рядом ходят зубы.
        if (showAnimals && !snapshot.animals.empty()) {
            std::vector<int> herbivores(std::max<std::size_t>(snapshot.herbivoreSpecies.size(), 1), 0);
            std::vector<int> predators(std::max<std::size_t>(snapshot.predatorSpecies.size(), 1), 0);
            std::map<std::string, int> desires;
            for (const auto& animal : snapshot.animals) {
                auto& population = animal.predator ? predators : herbivores;
                if (animal.species >= 0 && static_cast<std::size_t>(animal.species) < population.size()) {
                    ++population[static_cast<std::size_t>(animal.species)];
                }
                ++desires[animal.desire];
            }

            int legendX = 10;
            const int legendY = layerY + 40;
            for (std::size_t s = 0; s < herbivores.size(); ++s) {
                DrawRectangle(legendX, legendY + 1, 10, 10, TileColors::herbivoreSpecies(static_cast<int>(s)));
                const char* label = TextFormat("hb%zu %d", s, herbivores[s]);
                DrawText(label, legendX + 14, legendY, 14, layerOnColor);
                legendX += 14 + MeasureText(label, 14) + 12;
            }
            for (std::size_t s = 0; s < predators.size(); ++s) {
                DrawRectangle(legendX, legendY + 1, 10, 10, TileColors::predatorSpecies(static_cast<int>(s)));
                const char* label = TextFormat("pr%zu %d", s, predators[s]);
                DrawText(label, legendX + 14, legendY, 14, layerOnColor);
                legendX += 14 + MeasureText(label, 14) + 12;
            }
            for (const auto& [name, count] : desires) {
                const char* label = TextFormat("%s %d", name.c_str(), count);
                DrawText(label, legendX, legendY, 14, mutedColor);
                legendX += MeasureText(label, 14) + 12;
            }
        }
    }

    DrawRectangle(0, 0, viewportW, kHudHeight, hudColor);
    DrawText(TextFormat("%s   Tick: %llu   Area: %dx%d   Zoom: %d%%",
                         snapshot.currentWorld.empty() ? "(unsaved world)" : snapshot.currentWorld.c_str(),
                         static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                         static_cast<int>(std::round(zoom * 100.0f))),
             10, 8, 16, textColor);

    // Пока открыт любой из модальных диалогов, ни кнопки полосы, ни панель
    // настроек под ним не должны ловить клики.
    const bool modalOpen = confirmingExit || showSaveDialog;
    if (modalOpen) GuiLock();

    // Кнопки полосы — справа налево. Пауза здесь же кнопкой, а не только
    // клавишей P: на прежнем экране генерации Start/Stop нажимали мышью,
    // не отрываясь от ползунков.
    float buttonX = static_cast<float>(viewportW) - 110.0f;
    const bool backPressed = GuiButton(Rectangle{buttonX, 2, 100, kHudHeight - 4}, "Back (Esc)");
    buttonX -= 110.0f;
    const bool savePressed = GuiButton(Rectangle{buttonX, 2, 100, kHudHeight - 4}, "Save world");
    buttonX -= 110.0f;
    const bool panelPressed = GuiButton(Rectangle{buttonX, 2, 100, kHudHeight - 4}, panelOpen ? "Hide (P)" : "Panel (P)");
    buttonX -= 80.0f;
    const bool pausePressed = GuiButton(Rectangle{buttonX, 2, 70, kHudHeight - 4}, snapshot.paused ? "Start" : "Stop");

    if (savePressed) {
        std::snprintf(saveNameBuffer, sizeof(saveNameBuffer), "%s", snapshot.currentWorld.c_str());
        showSaveDialog = true;
    }
    if (panelPressed) {
        tab = panelOpen ? Tab::Hidden : Tab::Info;
        config.panel_tab = tabName(tab);
        goblins::saveClientConfig(configPath, config);
    }
    if (pausePressed) {
        network.sendTogglePause();
    }

    // Правая панель. Рисуется после карты и полосы, но до модальных
    // диалогов: она часть экрана, а не наложение поверх него.
    if (panelOpen) {
        DrawRectangleRec(panelBounds, Color{22, 22, 26, 255});

        // Полоса вкладок — своей отрисовкой, а не кнопками raygui: у
        // кнопки нет состояния "выбрана", а именно это и нужно показать.
        const Tab tabs[] = {Tab::Info, Tab::Params, Tab::Graphs};
        const char* labels[] = {"Info", "Params", "Graphs"};
        const float tabWidth = tabsBounds.width / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const Rectangle rect{tabsBounds.x + tabWidth * i, tabsBounds.y, tabWidth, tabsBounds.height};
            const bool active = tab == tabs[i];
            const bool hovered = !modalOpen && CheckCollisionPointRec(mouse, rect);
            DrawRectangleRec(rect, active ? Color{40, 42, 50, 255}
                                          : (hovered ? Color{30, 30, 36, 255} : hudColor));
            const int labelWidth = MeasureText(labels[i], 16);
            DrawText(labels[i], static_cast<int>(rect.x + rect.width * 0.5f) - labelWidth / 2,
                     static_cast<int>(rect.y) + 8, 16, active ? textColor : mutedColor);
            if (active) {
                // Подчёркивание снизу — та же роль, что у корешка открытой
                // вкладки: видно, какая из трёх сейчас перед глазами.
                DrawRectangle(static_cast<int>(rect.x), static_cast<int>(rect.y + rect.height) - 2,
                              static_cast<int>(rect.width), 2, cursorColor);
            }
            if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                tab = tabs[i];
                config.panel_tab = tabName(tab);
                goblins::saveClientConfig(configPath, config);
            }
        }

        switch (tab) {
            case Tab::Info:
                InfoPanel::draw(snapshot, infoTarget, panelContent);
                break;
            case Tab::Params: {
                goblins::RegenerationRequest panelParams;
                bool saveParamsRequested = false;
                // Панель параметров рисует собственную прокрутку и хочет
                // прямоугольник целиком — ей отдаётся всё под вкладками.
                const Rectangle paramsBounds{panelX, static_cast<float>(kHudHeight), kPanelWidth,
                                             static_cast<float>(screenH) - kHudHeight};
                if (panel.draw(paramsBounds, panelParams, saveParamsRequested)) {
                    network.sendRegenerate(panelParams);
                }
                if (saveParamsRequested) {
                    network.sendSaveGenerationConfig(panelParams);
                }
                break;
            }
            case Tab::Graphs:
                PopulationGraph::draw(snapshot, panelContent, !modalOpen);
                break;
            case Tab::Hidden:
                break;
        }
    }

    if (modalOpen) GuiUnlock();

    // Подсказка по клавишам — по нижнему краю слева, мелким приглушённым
    // текстом: в верхней полосе её теснят имя мира и кнопки, а нужна она
    // редко.
    DrawText("WASD-scroll  Wheel-zoom  F-fit  1-7-layers  Space-pause  P-panel  LMB-track  RMB-clear  C-constants  "
              "Esc-menu",
             10, screenH - 20, 14, mutedColor);

    const int bottomInfoY = screenH - 42;

    // Параметры тайла под курсором — по нижнему краю справа (над панелью
    // настроек, если она открыта, места нет — поэтому от края карты).
    if (hasHoverTile) {
        const std::size_t hi = static_cast<std::size_t>(hoverY) * snapshot.areaWidth + hoverX;
        const bool hoverIsSource =
            std::any_of(snapshot.waterSources.begin(), snapshot.waterSources.end(),
                        [&](const auto& s) { return s.first == hoverX && s.second == hoverY; });
        // Подпись собирается по кускам, а не одним TextFormat со вложенными
        // TextFormat внутри: у raylib на все вызовы всего четыре
        // прокручиваемых буфера (MAX_TEXTFORMAT_BUFFERS), и пятый
        // вложенный вызов затёр бы строку, которую внешний ещё не
        // прочитал. Здесь результат каждого вызова копируется в строку
        // сразу же, поэтому необязательных кусков может быть сколько
        // угодно.
        std::string tileLabel = TextFormat("Tile (%d,%d)  moist %.2f  rock %.2f  pack %.2f  min %d", hoverX, hoverY,
                                            snapshot.moisture[hi], snapshot.rockiness[hi], snapshot.compaction[hi],
                                            snapshot.minerals[hi]);
        if (snapshot.waterDepth[hi] > 0.0f) {
            tileLabel += TextFormat("  water %.2f", snapshot.waterDepth[hi]);
        }
        if (hoverIsSource) {
            tileLabel += "  [source]";
        }
        if (snapshot.plantSpeciesAt[hi] >= 0) {
            tileLabel +=
                TextFormat("  grass sp%d %.0f%%", snapshot.plantSpeciesAt[hi], snapshot.plantGrowth[hi] * 100.0f);
        }
        // Семя — отдельной пометкой, а не вместо травы: под стоящим
        // растением обычно лежит его же семя, и клетка одинаково честно и
        // занята, и засеяна.
        if (snapshot.seedSpeciesAt[hi] >= 0) {
            tileLabel += TextFormat("  seed sp%d", snapshot.seedSpeciesAt[hi]);
        }
        if (snapshot.humus[hi] > 0) {
            tileLabel += TextFormat("  humus %d", snapshot.humus[hi]);
        }
        const int labelWidth = MeasureText(tileLabel.c_str(), 16);
        DrawText(tileLabel.c_str(), viewportW - labelWidth - 12, bottomInfoY, 16, cursorColor);

        // Животные под курсором — отдельной строкой над строкой тайла: их
        // на клетке может быть несколько, и втискивать их в подпись самого
        // тайла (у которого всё по одному) значило бы врать о том, как
        // устроен мир.
        if (showAnimals) {
            std::string animalsLabel;
            if (!snapshot.carcass.empty() && snapshot.carcass[hi] > 0.0f) {
                animalsLabel += TextFormat("carcass %.1f", snapshot.carcass[hi]);
            }
            for (const auto& animal : snapshot.animals) {
                if (animal.x != hoverX || animal.y != hoverY) {
                    continue;
                }
                if (!animalsLabel.empty()) {
                    animalsLabel += "   ";
                }
                animalsLabel += TextFormat("%s%d %s %.0f%%%s -> %s", animal.predator ? "pr" : "hb", animal.species,
                                            animal.sex.c_str(), animal.growth * 100.0f,
                                            animal.health < 0.99f ? TextFormat(" hurt %.0f%%", animal.health * 100.0f)
                                                                  : "",
                                            animal.desire.c_str());
            }
            if (!animalsLabel.empty()) {
                const int animalsWidth = MeasureText(animalsLabel.c_str(), 16);
                DrawText(animalsLabel.c_str(), viewportW - animalsWidth - 12, bottomInfoY - 20, 16,
                         Color{235, 195, 120, 255});
            }
        }
    }

    // Результат сохранения (или ошибка загрузки, если мир так и не
    // открылся) — единственное место, где клиент об этом узнаёт.
    if (hasFreshNotice(snapshot)) {
        DrawText(snapshot.notice.c_str(), 10, bottomInfoY, 16,
                 snapshot.noticeIsError ? Color{230, 110, 110, 255} : textColor);
    }

    if (snapshot.paused) {
        const char* pausedLabel = "PAUSED";
        const int labelWidth = MeasureText(pausedLabel, 24);
        DrawRectangle(viewportW / 2 - labelWidth / 2 - 10, kHudHeight + 8, labelWidth + 20, 32, hudColor);
        DrawText(pausedLabel, viewportW / 2 - labelWidth / 2, kHudHeight + 12, 24, pausedColor);
    }

    // Выход всегда идёт через подтверждение: несохранённые изменения
    // мира иначе легко потерять случайным Esc/Back. Esc, открывший
    // диалог, не должен в тот же кадр его же и закрыть — поэтому это
    // if/else, а не независимые if с одним и тем же нажатием. Диалог
    // имени при сохранении имеет приоритет: Esc в нём просто отменяет
    // ввод имени, а не открывает диалог выхода.
    const bool escapePressed = IsKeyPressed(KEY_ESCAPE);
    if (showSaveDialog) {
        if (escapePressed) {
            showSaveDialog = false;
        }
    } else if (!confirmingExit) {
        if (backPressed || escapePressed) {
            confirmingExit = true;
        }
    } else if (escapePressed) {
        confirmingExit = false;
    }

    if (showSaveDialog) {
        DrawRectangle(0, 0, screenW, screenH, Color{0, 0, 0, 150});

        const int boxW = 380;
        const int boxH = 130;
        const int boxX = screenW / 2 - boxW / 2;
        const int boxY = screenH / 2 - boxH / 2;
        DrawRectangle(boxX, boxY, boxW, boxH, hudColor);
        DrawRectangleLines(boxX, boxY, boxW, boxH, textColor);
        DrawText("Save world as:", boxX + 20, boxY + 16, 18, textColor);

        GuiTextBox(Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 44, 340, 28}, saveNameBuffer,
                   sizeof(saveNameBuffer), true);

        const bool confirmSave = GuiButton(
            Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 84, 160, 30}, "Save");
        const bool cancelSave = GuiButton(
            Rectangle{static_cast<float>(boxX) + 200, static_cast<float>(boxY) + 84, 160, 30}, "Cancel (Esc)");

        if (confirmSave || (IsKeyPressed(KEY_ENTER) && std::strlen(saveNameBuffer) > 0)) {
            network.sendSaveWorld(saveNameBuffer);
            showSaveDialog = false;
        } else if (cancelSave) {
            showSaveDialog = false;
        }
    }

    if (confirmingExit) {
        DrawRectangle(0, 0, screenW, screenH, Color{0, 0, 0, 150});

        const int boxW = 380;
        const int boxH = 130;
        const int boxX = screenW / 2 - boxW / 2;
        const int boxY = screenH / 2 - boxH / 2;
        DrawRectangle(boxX, boxY, boxW, boxH, hudColor);
        DrawRectangleLines(boxX, boxY, boxW, boxH, textColor);
        DrawText("Save world before exiting?", boxX + 20, boxY + 16, 18, textColor);

        const bool saveExit =
            GuiButton(Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 56, 160, 30},
                      "Save & Exit");
        const bool discardExit =
            GuiButton(Rectangle{static_cast<float>(boxX) + 200, static_cast<float>(boxY) + 56, 160, 30},
                      "Discard & Exit");
        const bool cancelExit = GuiButton(
            Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 94, 340, 26}, "Cancel (Esc)");

        if (saveExit) {
            network.sendSaveWorld();
            network.sendStopSimulation();
            confirmingExit = false;
            return AppScreen::MainMenu;
        }
        if (discardExit) {
            network.sendStopSimulation();
            confirmingExit = false;
            return AppScreen::MainMenu;
        }
        if (cancelExit) {
            confirmingExit = false;
        }
    }

    // Оверлей констант — последним: он перекрывает всё, включая диалоги.
    // Переключение отключено, пока открыто поле ввода имени мира, иначе
    // буква "C" в имени открывала бы оверлей вместо того, чтобы набраться.
    ConstantsOverlay::update(snapshot, !showSaveDialog);

    return AppScreen::World;
}

} // namespace WorldScreen

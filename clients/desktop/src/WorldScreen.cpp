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
#include "KeysPanel.hpp"
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
// Нижняя полоса состояния: сообщения сервера и напоминание про панель.
// Карта заканчивается над ней — на самой карте надписей нет вовсе.
constexpr float kStatusHeight = 26.0f;

// Правая панель одна на три содержимого: карточка того, на что смотришь,
// параметры генерации и графики численности. Одно место, а не три угла
// экрана: смотрят в них по очереди, а места они просят одинаково много.
enum class Tab { Hidden, Info, Params, Graphs, Keys };

const char* tabName(Tab tab) {
    switch (tab) {
        case Tab::Info: return "info";
        case Tab::Params: return "params";
        case Tab::Graphs: return "graphs";
        case Tab::Keys: return "keys";
        case Tab::Hidden: break;
    }
    return "hidden";
}

Tab tabFromName(const std::string& name) {
    if (name == "info") return Tab::Info;
    if (name == "params") return Tab::Params;
    if (name == "graphs") return Tab::Graphs;
    if (name == "keys") return Tab::Keys;
    return Tab::Hidden;
}

// Порядок перебора клавишей P: три вкладки, затем свёрнутая панель. Свёрнутое
// состояние — часть того же круга, а не отдельная клавиша: панель и так
// закрывает треть экрана, и убрать её должно быть можно тем же движением,
// которым её листают.
// Просьба открыть вкладку параметров при следующем показе экрана (см.
// requestParamsTab в заголовке). Флаг, а не параметр draw: просит один
// экран (выбор мира), а исполняет другой, и между ними нет ничего, кроме
// возвращённого AppScreen.
bool g_paramsRequested = false;

Tab nextTab(Tab tab) {
    switch (tab) {
        case Tab::Info: return Tab::Params;
        case Tab::Params: return Tab::Graphs;
        case Tab::Graphs: return Tab::Keys;
        case Tab::Keys: return Tab::Hidden;
        case Tab::Hidden: break;
    }
    return Tab::Info;
}

} // namespace

void requestParamsTab() {
    g_paramsRequested = true;
}

AppScreen draw(NetworkClient& network, goblins::ClientConfig& config, const std::string& configPath,
               SettingsPanel& panel, bool& closeRequested) {
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
        showMoisture = config.show_moisture;
        showMinerals = config.show_minerals;
        showHeight = config.show_height;
        showPlants = config.show_plants;
        showAnimals = config.show_animals;
        tab = tabFromName(config.panel_tab);
        initialized = true;
    }
    // Игрок пришёл сюда с "New world" — мир ещё не создан, и первое, что
    // ему нужно, это параметры генерации.
    if (g_paramsRequested) {
        tab = Tab::Params;
        config.panel_tab = tabName(tab);
        goblins::saveClientConfig(configPath, config);
        g_paramsRequested = false;
    }
    static bool confirmingExit = false;
    // Диалог сохранения открыт из-за крестика, а не из-за Esc/Back: тогда
    // подтверждение закрывает приложение, а не возвращает в меню.
    static bool exitingApp = false;
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
    const int viewportH = screenH - kHudHeight - static_cast<int>(kStatusHeight);

    // Полоса вкладок — вровень с верхней полосой над картой: это один и тот
    // же ряд элементов управления, просто над разными половинами экрана.
    const Rectangle panelBounds{panelX, 0, kPanelWidth, static_cast<float>(screenH)};
    const Rectangle tabsBounds{panelX, 0, kPanelWidth, static_cast<float>(kHudHeight)};
    const Rectangle panelContent{panelX + 10.0f, static_cast<float>(kHudHeight) + 8.0f, kPanelWidth - 20.0f,
                                 static_cast<float>(screenH) - kHudHeight - 8.0f - kStatusHeight};

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
        // (каменистость/влажность/минералы считаются нулевыми, если слой
        // выключен). Вода — тот же выключатель, что и влажность
        // (KEY_TWO): вода на карте — это и есть источник влажности,
        // раздельные флаги только путали бы (можно было увидеть воду при
        // погашенном слое влажности). Высота (KEY_FOUR) — не часть
        // смешения, а множитель яркости поверх готового цвета (см.
        // TileColors::applyHeightShading), поэтому переключается и
        // применяется отдельно от остальных трёх.
        // Каждое переключение — редкое дискретное событие (не каждый
        // кадр, как WASD-прокрутка), поэтому сохраняем в config.json сразу,
        // без отдельной кнопки "Сохранить" — как и масштаб выше.
        if (IsKeyPressed(KEY_ONE)) {
            showRockiness = !showRockiness;
            config.show_rockiness = showRockiness;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_TWO)) {
            showMoisture = !showMoisture;
            config.show_moisture = showMoisture;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_THREE)) {
            showMinerals = !showMinerals;
            config.show_minerals = showMinerals;
            goblins::saveClientConfig(configPath, config);
        }
        if (IsKeyPressed(KEY_FOUR)) {
            showHeight = !showHeight;
            config.show_height = showHeight;
            goblins::saveClientConfig(configPath, config);
        }
        // Растения и перегной — один выключатель (KEY_FIVE): перегной это
        // и есть след умершего растения, разделять их значило бы видеть
        // остатки при погашенном слое травы.
        if (IsKeyPressed(KEY_FIVE)) {
            showPlants = !showPlants;
            config.show_plants = showPlants;
            goblins::saveClientConfig(configPath, config);
        }
        // Животные — отдельный выключатель, а не часть слоя травы: это не
        // слой почвы, а объекты поверх карты (как булыжники и источники),
        // и смотреть на луг без зверей — обычное дело. Падаль гаснет
        // вместе с ними: она их след, а не свойство почвы.
        if (IsKeyPressed(KEY_SIX)) {
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

    // Набор включённых слоёв нужен и карте, и вкладке Keys (она показывает
    // их состояние), поэтому собирается до того, как что-либо рисуется.
    MapTexture::Layers layers;
    layers.rockiness = showRockiness;
    layers.moisture = showMoisture;
    layers.minerals = showMinerals;
    layers.height = showHeight;
    layers.plants = showPlants;
    layers.animals = showAnimals;

    if (!snapshot.connected || snapshot.areaWidth == 0) {
        const std::string waiting = "Connecting to " + config.host + ":" + std::to_string(config.port) + "...";
        DrawText(waiting.c_str(), 10, kHudHeight + 10, 20, textColor);
    } else {
        BeginScissorMode(0, kHudHeight, viewportW, viewportH);

        // Карта — одна текстура (тексель на тайл), пересобирается только
        // при новом состоянии мира или смене набора слоёв; за кадр это
        // один вызов отрисовки вместо тысяч прямоугольников, а отсечение
        // невидимой части делает сам ножничный режим.
        const Texture2D& mapTexture = mapCache.texture(snapshot, layers);
        DrawTexturePro(mapTexture,
                       Rectangle{0, 0, static_cast<float>(snapshot.areaWidth),
                                 static_cast<float>(snapshot.areaHeight)},
                       Rectangle{-viewX, kHudHeight - viewY, static_cast<float>(snapshot.areaWidth) * tileSizeF,
                                 static_cast<float>(snapshot.areaHeight) * tileSizeF},
                       Vector2{0, 0}, 0.0f, WHITE);

        // Охота выбранного хищника — глазами самого хищника (см. "watched"
        // в протоколе; считает её мир, core/Hunting.hpp, клиент только
        // рисует пришедшие клетки). Округа — подсветкой под всем остальным:
        // это ответ на вопрос "почему он не пошёл за той добычей, что стоит
        // на виду за рекой", и заслонять ею карту нельзя.
        const bool watchingHunter = snapshot.watched.kind == "animal" && !snapshot.watched.reach.empty();
        if (watchingHunter) {
            for (const auto& cell : snapshot.watched.reach) {
                const float screenX = static_cast<float>(cell.first) * tileSizeF - viewX;
                const float screenY = static_cast<float>(cell.second) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY), std::max(1, tileSize),
                              std::max(1, tileSize), Color{90, 170, 255, 38});
            }
        }

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

        // Дорога — поверх зверей: она и есть то, ради чего смотрят. Линия
        // от самого хищника через клетки найденной дороги до цели, а на
        // цели — кольцо. Цвет говорит, за чем он идёт: за живой добычей или
        // к туше. Дороги нет вовсе — не нарисовано ничего, и это тоже
        // ответ: значит, идти хищнику не за кем.
        if (watchingHunter && !snapshot.watched.road.empty()) {
            // "teeth" — та же живая добыча, просто уже под зубами.
            const bool huntingPrey =
                snapshot.watched.roadKind == "prey" || snapshot.watched.roadKind == "teeth";
            const Color roadColor = huntingPrey ? Color{255, 120, 90, 235} : Color{235, 225, 170, 225};
            const float thickness = std::max(1.5f, tileSizeF * 0.14f);
            auto centerOf = [&](int cellX, int cellY) {
                return Vector2{static_cast<float>(cellX) * tileSizeF - viewX + tileSizeF * 0.5f,
                               static_cast<float>(cellY) * tileSizeF - viewY + kHudHeight + tileSizeF * 0.5f};
            };
            Vector2 from = centerOf(snapshot.watched.x, snapshot.watched.y);
            for (const auto& cell : snapshot.watched.road) {
                const Vector2 to = centerOf(cell.first, cell.second);
                DrawLineEx(from, to, thickness, roadColor);
                from = to;
            }
            const Vector2 goal = centerOf(snapshot.watched.roadX, snapshot.watched.roadY);
            DrawCircleLines(static_cast<int>(goal.x), static_cast<int>(goal.y), std::max(3.0f, tileSizeF * 0.45f),
                             roadColor);
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

        // Ни легенд, ни статуса слоёв, ни подписи тайла поверх карты
        // больше нет: карта — это картина мира, и текст по ней читается
        // хуже и её, и себя. Всё, что раньше стояло на ней надписями,
        // живёт в правой панели: что за клетка и кто на ней — во вкладке
        // Info, численность видов — в Graphs, состояние слоёв и сами
        // клавиши — в Keys. На карте остались только метки, а не слова:
        // рамка под курсором и кольцо на том, за кем следят.
    }

    DrawRectangle(0, 0, viewportW, kHudHeight, hudColor);
    const std::string hudLabel =
        TextFormat("%s   Tick: %llu   Area: %dx%d   Zoom: %d%%",
                   snapshot.currentWorld.empty() ? "(unsaved world)" : snapshot.currentWorld.c_str(),
                   static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                   static_cast<int>(std::round(zoom * 100.0f)));
    DrawText(hudLabel.c_str(), 10, 8, 16, textColor);

    // Остановленное время — в верхней полосе, следом за номером тика, а не
    // надписью поверх карты: это состояние мира, и место ему там же, где
    // остальное состояние мира. Заодно оно всегда на одном месте, а не
    // посреди того, на что игрок в этот момент смотрит.
    if (snapshot.paused) {
        DrawText("PAUSED", 10 + MeasureText(hudLabel.c_str(), 16) + 20, 8, 16, pausedColor);
    }

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
        const Tab tabs[] = {Tab::Info, Tab::Params, Tab::Graphs, Tab::Keys};
        const char* labels[] = {"Info", "Params", "Graphs", "Keys"};
        constexpr int kTabCount = 4;
        const float tabWidth = tabsBounds.width / static_cast<float>(kTabCount);
        for (int i = 0; i < kTabCount; ++i) {
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
                if (panel.draw(paramsBounds, panelParams, saveParamsRequested, snapshot.generated)) {
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
            case Tab::Keys:
                KeysPanel::draw(panelContent, layers);
                break;
            case Tab::Hidden:
                break;
        }
    }

    if (modalOpen) GuiUnlock();

    // Нижняя полоса — своя, а не надпись поверх карты: карта заканчивается
    // над ней (см. viewportH). Здесь живут сообщения сервера — результат
    // сохранения и ошибки загрузки, единственное место, где клиент о них
    // узнаёт, — и напоминание, где искать всё остальное.
    DrawRectangle(0, screenH - static_cast<int>(kStatusHeight), viewportW, static_cast<int>(kStatusHeight), hudColor);
    if (hasFreshNotice(snapshot)) {
        DrawText(snapshot.notice.c_str(), 10, screenH - 19, 14,
                 snapshot.noticeIsError ? Color{230, 110, 110, 255} : textColor);
    }
    const char* panelHint = "P -- panel: Info / Params / Graphs / Keys";
    DrawText(panelHint, viewportW - MeasureText(panelHint, 14) - 10, screenH - 19, 14, mutedColor);

    // Мира ещё нет: сервер поднялся с пустой Областью или игрок пришёл
    // сюда с "New world". Пустая карта сама по себе об этом не говорит —
    // она выглядит как ровная земля, — поэтому говорим прямо и там, где
    // на неё смотрят.
    if (snapshot.connected && !snapshot.generated) {
        const char* firstLine = "This world has not been generated yet.";
        const char* secondLine = tab == Tab::Params ? "Set the parameters on the right and press \"Create world\"."
                                                     : "Press P to open the generation parameters.";
        const int firstWidth = MeasureText(firstLine, 20);
        const int secondWidth = MeasureText(secondLine, 16);
        const int boxWidth = std::max(firstWidth, secondWidth) + 40;
        const int boxX = viewportW / 2 - boxWidth / 2;
        const int boxY = kHudHeight + viewportH / 2 - 40;
        DrawRectangle(boxX, boxY, boxWidth, 80, Color{18, 18, 20, 235});
        DrawRectangleLines(boxX, boxY, boxWidth, 80, Color{90, 90, 98, 255});
        DrawText(firstLine, viewportW / 2 - firstWidth / 2, boxY + 20, 20, textColor);
        DrawText(secondLine, viewportW / 2 - secondWidth / 2, boxY + 48, 16, mutedColor);
    }

    // Выход всегда идёт через подтверждение: несохранённые изменения
    // мира иначе легко потерять случайным Esc/Back. Esc, открывший
    // диалог, не должен в тот же кадр его же и закрыть — поэтому это
    // if/else, а не независимые if с одним и тем же нажатием. Диалог
    // имени при сохранении имеет приоритет: Esc в нём просто отменяет
    // ввод имени, а не открывает диалог выхода.
    // Крестик окна раylib само окно не закрывает — он только поднимает
    // флаг (см. main.cpp), и здесь этот флаг превращается в тот же вопрос
    // о сохранении, что и при выходе в меню. Иначе несохранённый мир
    // терялся бы молча, а мир — это часы прожитого времени.
    if (closeRequested) {
        closeRequested = false;
        confirmingExit = true;
        exitingApp = true;
        // Ввод имени поверх вопроса о выходе только запутал бы: имя
        // спросят снова, если игрок выберет "Save".
        showSaveDialog = false;
    }

    const bool escapePressed = IsKeyPressed(KEY_ESCAPE);
    if (showSaveDialog) {
        if (escapePressed) {
            showSaveDialog = false;
        }
    } else if (!confirmingExit) {
        if (backPressed || escapePressed) {
            confirmingExit = true;
            exitingApp = false;
        }
    } else if (escapePressed) {
        confirmingExit = false;
        exitingApp = false;
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
        DrawText(exitingApp ? "Save world before quitting?" : "Save world before exiting?", boxX + 20, boxY + 16, 18,
                 textColor);

        const bool saveExit =
            GuiButton(Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 56, 160, 30},
                      exitingApp ? "Save & Quit" : "Save & Exit");
        const bool discardExit =
            GuiButton(Rectangle{static_cast<float>(boxX) + 200, static_cast<float>(boxY) + 56, 160, 30},
                      exitingApp ? "Discard & Quit" : "Discard & Exit");
        const bool cancelExit = GuiButton(
            Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 94, 340, 26}, "Cancel (Esc)");

        // Мир сохраняет сервер, между тиками, и ответа клиент не ждёт: и
        // при выходе в меню, и при закрытии окна порядок один и тот же —
        // попросить сохранить, попросить остановить мир, уйти.
        if (saveExit || discardExit) {
            if (saveExit) {
                network.sendSaveWorld();
            }
            network.sendStopSimulation();
            const bool quit = exitingApp;
            confirmingExit = false;
            exitingApp = false;
            return quit ? AppScreen::Exit : AppScreen::MainMenu;
        }
        if (cancelExit) {
            confirmingExit = false;
            exitingApp = false;
        }
    }

    // Оверлей констант — последним: он перекрывает всё, включая диалоги.
    // Переключение отключено, пока открыто поле ввода имени мира, иначе
    // буква "C" в имени открывала бы оверлей вместо того, чтобы набраться.
    ConstantsOverlay::update(snapshot, !showSaveDialog);

    return AppScreen::World;
}

} // namespace WorldScreen

#include "WorldScreen.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <raygui.h>
#include <raylib.h>

#include "BuildSprites.hpp"
#include "ConstantsOverlay.hpp"
#include "DeerSprites.hpp"
#include "GenomeGraph.hpp"
#include "GoblinSprites.hpp"
#include "InfoPanel.hpp"
#include "KeysPanel.hpp"
#include "MapTexture.hpp"
#include "SelectionCard.hpp"
#include "WaterSprites.hpp"
#include "PlantSprites.hpp"
#include "TreeSprites.hpp"
#include "WolfSprites.hpp"
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
enum class Tab { Hidden, Info, Params, Graphs, Genome, Keys };

const char* tabName(Tab tab) {
    switch (tab) {
        case Tab::Info: return "info";
        case Tab::Params: return "params";
        case Tab::Graphs: return "graphs";
        case Tab::Genome: return "genome";
        case Tab::Keys: return "keys";
        case Tab::Hidden: break;
    }
    return "hidden";
}

Tab tabFromName(const std::string& name) {
    if (name == "info") return Tab::Info;
    if (name == "params") return Tab::Params;
    if (name == "graphs") return Tab::Graphs;
    if (name == "genome") return Tab::Genome;
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
        case Tab::Graphs: return Tab::Genome;
        case Tab::Genome: return Tab::Keys;
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
    // Перетаскивание левой кнопкой мыши — вторая прокрутка, рядом с WASD.
    // "Перетаскивание" отличается от обычного клика по клетке порогом
    // сдвига от точки нажатия (см. ниже, у обработки мыши): пока порог не
    // пройден, это ещё может оказаться кликом, и решение откладывается до
    // отпускания кнопки.
    static bool leftDragging = false;
    static Vector2 leftPressPos{};
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
    static bool showGoblins = true;
    static bool showTrampled = true;
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
        showGoblins = config.show_goblins;
        showTrampled = config.show_trampled;
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
    // "Save & Quit" не закрывает приложение в тот же кадр, что отправляет
    // save_world: main.cpp следом рвёт TCP-соединение (network.disconnect в
    // конце main после короткой паузы "на всякий случай"), а send() у
    // IXWebSocket не гарантирует, что байты успели уйти на провод именно
    // к этому моменту — эмпирически не успевали, и мир не сохранялся,
    // хотя диалог честно спрашивал и вызывал sendSaveWorld(). Раз в
    // протоколе уже есть подтверждение (notice после обработки save_world
    // на сервере, см. NetworkServer.hpp), ждём настоящего ответа вместо
    // догадки по времени: экран остаётся открытым до свежего notice (или
    // до истечения запасного таймаута, если сервер не отвечает вовсе —
    // приложение не должно виснуть на выходе).
    static bool waitingToSaveBeforeQuit = false;
    static std::chrono::steady_clock::time_point waitingSince{};
    static std::chrono::steady_clock::time_point waitingNoticeBaseline{};
    // Регенерация уничтожает текущий мир безвозвратно — как и выход,
    // спрашивает про сохранение первым делом, тем же диалогом по форме.
    // Спрашивать нечего только пока мира ещё нет вовсе (кнопка "Create
    // world" на пустом месте, см. ниже, у обработки панели) — тогда
    // терять нечего, и регенерация уходит сразу. Параметры с панели
    // запоминаются на время вопроса: сама панель к моменту ответа могла
    // уже перерисоваться с другими значениями.
    static bool confirmingRegenerate = false;
    static goblins::RegenerationRequest pendingRegenerateParams{};
    // Карта в текстуре — тоже состояние экрана: пересобирается только
    // когда пришло новое состояние мира или переключён слой.
    static MapTexture::Cache mapCache;
    // Вписать карту в окно по клавише F: сам пересчёт масштаба возможен
    // только когда известен размер Области, а он приходит с сервера —
    // поэтому нажатие запоминается здесь и отрабатывается ниже, после
    // получения снапшота.
    bool fitRequested = false;
    // Центрировать камеру на выбранном по клавише T — тем же способом, по
    // той же причине: положение существа известно только из снапшота
    // (оно ходит), поэтому нажатие запоминается здесь и отрабатывается
    // ниже.
    bool recenterRequested = false;

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
    constexpr float kScrollTilesPerSecond = 15.0f;
    const float scrollSpeedPx = kScrollTilesPerSecond * tileSizeF;

    const Vector2 mouse = GetMousePosition();
    // Карточка выбранного лежит НА карте, и место её нужно знать до всякого
    // ввода: щелчок по ней не должен ни выбирать клетку под нею, ни таскать
    // карту. Выбор берётся тот, что был на прошлом кадре, — и это верно: на
    // экране сейчас нарисована именно та карточка.
    const Rectangle cardBounds = SelectionCard::bounds(
        selection, Rectangle{0.0f, static_cast<float>(kHudHeight), static_cast<float>(viewportW),
                             static_cast<float>(viewportH)});
    const bool mouseOverCard = cardBounds.width > 0.0f && CheckCollisionPointRec(mouse, cardBounds);
    // Колесо над правой панелью прокручивает её саму (GuiScrollPanel) и
    // читает точку на графике — но не меняет масштаб карты: одно движение
    // колеса не должно делать сразу два несвязанных действия.
    const bool mouseOverMap = mouse.x < static_cast<float>(viewportW) && !mouseOverCard;

    // Пока открыт диалог подтверждения выхода или диалог сохранения, мир
    // под ним не должен реагировать на ввод (прокрутка/зум/слои/пауза) —
    // иначе клик по кнопке диалога совпадёт с движением камеры или сменой
    // слоя позади, а буквенные клавиши при вводе имени — с WASD/P/1-6.
    // Оверлей констант — по той же причине: он перекрывает экран целиком,
    // и слои под ним переключались бы вслепую.
    const bool inputBlocked =
        confirmingExit || confirmingRegenerate || waitingToSaveBeforeQuit || ConstantsOverlay::visible();

    // Перетаскивание левой кнопкой мыши. Порог сдвига от точки нажатия
    // решает, был ли это клик (выбор клетки, дальше в этом файле) или
    // перетаскивание (прокрутка): меньше порога — ещё возможный клик, и
    // выбор срабатывает по отпусканию кнопки, а не по нажатию — раньше
    // нельзя было отличить одно от другого, не дождавшись самого
    // отпускания. Больше порога хотя бы раз — дальше это перетаскивание
    // до самого отпускания, даже если мышь на миг вернётся к точке
    // нажатия.
    constexpr float kDragThresholdPx = 4.0f;
    bool leftClickSelects = false;
    if (!inputBlocked) {
        if (mouseOverMap && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            leftDragging = false;
            leftPressPos = mouse;
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (!leftDragging) {
                const float dx = mouse.x - leftPressPos.x;
                const float dy = mouse.y - leftPressPos.y;
                leftDragging = (dx * dx + dy * dy) > kDragThresholdPx * kDragThresholdPx;
            }
            if (leftDragging) {
                // Дельта за кадр, а не разница с точкой нажатия: второе
                // легло бы поверх уже накопленной прокрутки и требовало
                // бы своего собственного учёта границ карты вместо того,
                // чтобы довериться общей проверке ниже (та же, что и у
                // WASD).
                const Vector2 delta = GetMouseDelta();
                viewX -= delta.x;
                viewY -= delta.y;
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            leftClickSelects = !leftDragging;
            leftDragging = false;
        }
    }

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

            // По вертикали вычитается не сама mouse.y, а отступ карты от неё:
            // окно карты начинается под шапкой, и мировая точка считается от
            // её верха (см. worldY выше). Без этой поправки каждый щелчок
            // колеса уводил карту на высоту шапки, а так как следующий
            // щелчок множит уже уехавшее, за десяток щелчков цель уходила с
            // экрана — то самое "убегание", которого требовалось избежать.
            viewX = worldX * factor - mouse.x;
            viewY = worldY * factor - (mouse.y - kHudHeight);

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

        // Вернуть камеру к выбранному — на случай, если он ушёл (или его
        // проскроллили) за край видимого. Клик заново центрирует камеру
        // сам (см. ниже), эта клавиша — для той же цели без нового клика,
        // который по невидимому существу и не поставить.
        if (IsKeyPressed(KEY_T)) {
            recenterRequested = true;
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
        // Гоблины — свой выключатель: смотреть на мир без поселенцев и
        // смотреть на мир без зверья — разные надобности, и гасить их одной
        // клавишей значило бы не дать посмотреть ни на то, ни на другое
        // отдельно.
        if (IsKeyPressed(KEY_SEVEN)) {
            showGoblins = !showGoblins;
            config.show_goblins = showGoblins;
            goblins::saveClientConfig(configPath, config);
        }
        // Тропы — свой выключатель: смотреть на мир без них надо ровно
        // затем, чтобы увидеть, где они появились.
        if (IsKeyPressed(KEY_EIGHT)) {
            showTrampled = !showTrampled;
            config.show_trampled = showTrampled;
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

        // Быстрое сохранение — без диалога и без имени: сервер сам решает,
        // куда писать (см. NetworkServer::handleClientMessage, "save_world"
        // с пустым именем) — под именем текущего мира, а если он ещё ни
        // разу не сохранялся, придумывает новое. Результат виден в полосе
        // состояния снизу (notice), поэтому отдельного подтверждения на
        // экране не нужно. Кнопка "Save world" убрана — это единственный
        // способ сохранить мир руками, не считая выхода с экрана.
        const bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrlDown && IsKeyPressed(KEY_S)) {
            network.sendSaveWorld();
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

    // Текущая клетка цели: для существа — по идентификатору (оно ходит, и
    // клетка, с которой его выбрали, уже не его клетка), для травы и
    // почвы — сама клетка запроса. Пусто, если существа, за которым
    // следили, больше нет в списке (умерло, съедено) — центрировать тогда
    // не на чем, и marker на карте по той же причине не рисуется.
    const auto markOf = [&snapshot](const InfoPanel::Target& target) -> std::optional<std::pair<int, int>> {
        if (target.kind == InfoPanel::Target::Kind::None) {
            return std::nullopt;
        }
        if (target.kind == InfoPanel::Target::Kind::Goblin) {
            for (const auto& goblin : snapshot.goblins) {
                if (goblin.id == target.animalId) {
                    return std::make_pair(goblin.x, goblin.y);
                }
            }
            return std::nullopt;
        }
        if (target.kind != InfoPanel::Target::Kind::Animal) {
            return std::make_pair(target.x, target.y);
        }
        for (const auto& animal : snapshot.animals) {
            if (animal.id == target.animalId) {
                return std::make_pair(animal.x, animal.y);
            }
        }
        return std::nullopt;
    };

    // Поставить камеру так, чтобы клетка (x, y) оказалась в центре
    // видимой части карты, и тут же вписать результат в границы мира —
    // тем же способом, что и обычная прокрутка чуть выше, просто разом
    // вместо покадрового приближения.
    const auto centerOn = [&](int x, int y) {
        viewX = (static_cast<float>(x) + 0.5f) * tileSizeF - static_cast<float>(viewportW) * 0.5f;
        viewY = (static_cast<float>(y) + 0.5f) * tileSizeF - static_cast<float>(viewportH) * 0.5f;
        const float maxX = std::max(0.0f, static_cast<float>(snapshot.areaWidth) * tileSizeF - viewportW);
        const float maxY = std::max(0.0f, static_cast<float>(snapshot.areaHeight) * tileSizeF - viewportH);
        viewX = std::clamp(viewX, 0.0f, maxX);
        viewY = std::clamp(viewY, 0.0f, maxY);
    };

    // Клавиша T (запомнена выше, до снапшота: положение существа известно
    // только теперь).
    if (recenterRequested) {
        if (const auto mark = markOf(selection)) {
            centerOn(mark->first, mark->second);
        }
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
        // Гоблины — в том же переборе и перед растением: клик по клетке, где
        // стоит поселенец, должен показывать его, а не землю под ним.
        for (const auto& goblin : snapshot.goblins) {
            if (goblin.x == x && goblin.y == y) {
                targets.push_back(InfoPanel::Target{InfoPanel::Target::Kind::Goblin, goblin.id, x, y, true});
            }
        }
        const std::size_t index = static_cast<std::size_t>(y) * snapshot.areaWidth + x;
        // Дерево выбирается тем же кликом и тем же видом цели, что и трава:
        // растение на клетке одно, а какое именно — разбирается InfoPanel.
        if ((index < snapshot.plantSpeciesAt.size() && snapshot.plantSpeciesAt[index] >= 0) ||
            (index < snapshot.treeSpeciesAt.size() && snapshot.treeSpeciesAt[index] >= 0)) {
            targets.push_back(InfoPanel::Target{InfoPanel::Target::Kind::Plant, 0, x, y, true});
        }
        targets.push_back(InfoPanel::Target{InfoPanel::Target::Kind::Soil, 0, x, y, true});
        return targets;
    };

    if (leftClickSelects && hasHoverTile) {
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
        // Камера — на выбранное: клик по клетке у самого края экрана
        // (обычный способ заметить кого-то на подходе) иначе оставлял бы
        // выбранного на самом краю, а не там, где удобно разглядывать.
        if (const auto mark = markOf(selection)) {
            centerOn(mark->first, mark->second);
        }
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
                                : infoTarget.kind == InfoPanel::Target::Kind::Goblin ? "goblin"
                                : infoTarget.kind == InfoPanel::Target::Kind::Plant  ? "plant"
                                                                                     : "none";
        const std::uint64_t watchId = (infoTarget.kind == InfoPanel::Target::Kind::Animal ||
                                       infoTarget.kind == InfoPanel::Target::Kind::Goblin)
                                           ? infoTarget.animalId
                                           : 0;
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
    layers.goblins = showGoblins;
    layers.trampled = showTrampled;

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

        const std::size_t cellCount =
            static_cast<std::size_t>(snapshot.areaWidth) * static_cast<std::size_t>(snapshot.areaHeight);

        // Рябь на воде — сразу за текстурой карты и раньше всего остального:
        // она не предмет НА воде, а сама её поверхность, и заслонять ею
        // подсветку, булыжник или зверя было бы враньём.
        //
        // Порог тот же, что у травы (десять пикселей на клетку), и по той же
        // причине: чёрточка ряби шириной в пиксель, и на клетке мельче
        // половина их выпадает вовсе — выходит не вода, а сор. Ниже порога
        // клетка остаётся тем, чем она нарисована в текстуре карты, — цветом
        // по глубине, и это та же вода, изображённая настолько подробно,
        // насколько её видно.
        //
        // Слой тот же, что и у самой воды в текстуре (showMoisture): выключив
        // воду, выключаешь и её рябь.
        const auto waterDetail = WaterSprites::detailFor(tileSizeF);
        const bool drawRipples = showMoisture && tileSize >= 10 && WaterSprites::ready(waterDetail) &&
                                 snapshot.waterDepth.size() >= cellCount &&
                                 snapshot.height.size() >= cellCount;
        if (drawRipples) {
            const int firstX = std::max(0, static_cast<int>(std::floor(viewX / tileSizeF)));
            const int lastX = std::min(snapshot.areaWidth - 1,
                                       static_cast<int>(std::floor((viewX + viewportW) / tileSizeF)));
            const int firstY = std::max(0, static_cast<int>(std::floor(viewY / tileSizeF)));
            const int lastY = std::min(snapshot.areaHeight - 1,
                                       static_cast<int>(std::floor((viewY + viewportH) / tileSizeF)));
            for (int y = firstY; y <= lastY; ++y) {
                for (int x = firstX; x <= lastX; ++x) {
                    const std::size_t cell = static_cast<std::size_t>(y) * snapshot.areaWidth + x;
                    if (snapshot.waterDepth[cell] <= 0.0f) {
                        continue;
                    }
                    // Направление считается на каждый видимый тайл каждый
                    // кадр, а не запасается на снимок. Восемь соседей на
                    // тайл — это десятки тысяч сложений в кадр, то есть
                    // ничто; запас же пришлось бы сбрасывать при каждой
                    // дельте, а вода меняется каждый тик, и запас всё равно
                    // считался бы заново — только с лишним поводом
                    // разъехаться со снимком.
                    const WaterSprites::Flow flow = WaterSprites::flowAt(
                        snapshot.height, snapshot.waterDepth, snapshot.areaWidth, snapshot.areaHeight, x, y);
                    DrawTexturePro(WaterSprites::atlas(waterDetail),
                                   WaterSprites::source(waterDetail,
                                                        WaterSprites::depthStep(snapshot.waterDepth[cell]),
                                                        flow, WaterSprites::variantOf(x, y),
                                                        WaterSprites::frameOf(flow, snapshot.tick)),
                                   Rectangle{static_cast<float>(x) * tileSizeF - viewX,
                                             static_cast<float>(y) * tileSizeF - viewY + kHudHeight,
                                             tileSizeF, tileSizeF},
                                   Vector2{0, 0}, 0.0f, WHITE);
                }
            }
        }

        // Дорога выбранного зверя — его же глазами (см. "watched" в
        // протоколе; считает её мир — core/Path.hpp, — а клиент только
        // рисует пришедшие клетки). Округа, до которой у него есть ход, —
        // подсветкой под всем остальным: это ответ на вопрос "почему он не
        // пошёл к тому, что стоит на виду за рекой", и заслонять ею карту
        // нельзя.
        // Пригодность округи для отдыха у наблюдаемого гоблина — тем же
        // способом и в том же месте, что и округа зверя: подсветкой ПОД
        // всем остальным. Это ответ на вопрос "почему он лёг именно здесь",
        // и заслонять ею карту нельзя.
        //
        // Зелёное — годное для отдыха (порог решает сам мир, core/Rest.hpp),
        // и чем годнее, тем плотнее; негодное не рисуется вовсе. Не шкала
        // от нуля до единицы: гоблину важно не "насколько тут хорошо", а
        // "годится ли", — и карта должна отвечать на тот же вопрос.
        if (snapshot.watched.kind == "goblin" && !snapshot.watched.rest.empty()) {
            for (const auto& [cellX, cellY, quality] : snapshot.watched.rest) {
                if (quality < 45) {
                    continue; // ниже порога мира (kRestGood в сотых)
                }
                const float screenX = static_cast<float>(cellX) * tileSizeF - viewX;
                const float screenY = static_cast<float>(cellY) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                // От порога до сотни — в прозрачность: у самого порога
                // клетка едва заметна, у лучшей видна ясно.
                const int alpha = 40 + (quality - 45) * 90 / 55;
                DrawRectangle(static_cast<int>(screenX), static_cast<int>(screenY),
                              std::max(1, static_cast<int>(tileSizeF)), std::max(1, static_cast<int>(tileSizeF)),
                              Color{110, 200, 150, static_cast<unsigned char>(std::clamp(alpha, 0, 160))});
            }
        }

        const bool watchingWalker = snapshot.watched.kind == "animal" && !snapshot.watched.reach.empty();
        if (watchingWalker) {
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

        // Всё, что на клетке СТОИТ, — деревья и постройки, — одним обходом
        // по строкам сверху вниз.
        //
        // Клеткой ни то, ни другое не выражается: они из неё торчат вверх, и
        // в этом вся разница между деревом и травой, между навесом и
        // утоптанной землёй. Трава и утоптанность — свойства земли, их место
        // в текстуре карты; дерево и навес на земле стоят.
        //
        // Обход по строкам сверху вниз, и это не всё равно: стоящее в нижней
        // строке заходит на верхнюю и рисоваться должно ПОВЕРХ тамошнего —
        // иначе роща на склоне выглядела бы вывернутой наизнанку. Нижняя
        // граница обхода на строку больше видимой ровно поэтому же.
        //
        // Обход ОДИН на оба, и это не мелочь: двумя проходами всякий навес
        // оказался бы либо над всяким деревом, либо под всяким, а глубина
        // здесь задаётся строкой, а не тем, чем предмет является.
        const bool drawTrees = showPlants && !snapshot.treeSpeciesAt.empty();
        // Рисунок постройки — те же шестнадцать пикселей на клетку, что и у
        // дерева, и запасного пути у него нет: на мелком масштабе постройка
        // остаётся тем, чем она нарисована в самой текстуре карты, — оттенком
        // клетки (TileColors::canopy). Тот же предмет, изображённый настолько
        // подробно, насколько его видно.
        const bool drawBuildings = showGoblins && tileSize >= 6 && BuildSprites::ready() &&
                                   snapshot.canopy.size() >= cellCount && snapshot.bedding.size() >= cellCount &&
                                   snapshot.site.size() >= cellCount;
        // Трава и куст — тем же обходом, что дерево и постройка, но по другой
        // причине: заслонять им нечего, из клетки они не торчат, и порядок
        // строк им безразличен. Обход общий просто потому, что он один и уже
        // идёт по видимым клеткам, а второй такой же стоил бы ровно вдвое.
        //
        // Порог выше древесного (десять против шести): у былинки ширина в
        // пиксель, и на клетке мельче десяти половина их выпадает вовсе —
        // выходит не трава, а сор. Ниже порога клетка остаётся тем, чем она
        // нарисована в текстуре карты, — подмешкой цвета, и это тот же луг,
        // изображённый настолько подробно, насколько его видно.
        const bool drawPlantSprites = showPlants && tileSize >= 10 &&
                                      snapshot.plantSpeciesAt.size() >= cellCount &&
                                      snapshot.plantGrowth.size() >= cellCount;
        const auto grassDetail = PlantSprites::grassDetailFor(tileSizeF);
        const auto bushDetail = PlantSprites::bushDetailFor(tileSizeF);
        const bool drawGrass = drawPlantSprites && PlantSprites::grassReady(grassDetail);
        const bool drawBushes = drawPlantSprites && PlantSprites::bushReady(bushDetail) &&
                                snapshot.bushSpeciesAt.size() >= cellCount;
        if (drawTrees || drawBuildings || drawGrass || drawBushes) {
            const int firstX = std::max(0, static_cast<int>(std::floor(viewX / tileSizeF)));
            const int lastX = std::min(snapshot.areaWidth - 1,
                                        static_cast<int>(std::floor((viewX + viewportW) / tileSizeF)));
            const int firstY = std::max(0, static_cast<int>(std::floor(viewY / tileSizeF)));
            const int lastY = std::min(snapshot.areaHeight - 1,
                                        static_cast<int>(std::floor((viewY + viewportH) / tileSizeF)) + 1);
            // Рисунок дерева — шестнадцать пикселей на клетку (TreeSprites).
            // Когда клетка сама меньше шести пикселей (карта, вписанная в
            // окно), от него остаётся каша: рисуем тогда тем, чем рисовали
            // до спрайтов — тонким стволом в треть клетки. Это не запасной
            // путь на случай беды, а тот же предмет, изображённый настолько
            // подробно, насколько его видно.
            // Подробность у каждого рисунка своя: у дерева кадр вдвое выше, у
            // травы с кустом — ровно клетка, и крупный лист у одного может
            // быть, а у другого нет.
            const auto treeDetail = TreeSprites::detailFor(tileSizeF);
            const bool drawSprites = tileSize >= 6 && TreeSprites::ready(treeDetail);
            const int trunkWidth = std::max(1, tileSize / 3);
            const int trunkHeight = std::max(1, tileSize * 2);
            // Время берётся раз на всю рощу: качание считается от него и от
            // клетки, и один и тот же миг для всех деревьев кадра —
            // единственное, что тут обязано совпадать.
            const double now = GetTime();
            // Место рисунка: своя клетка и клетка над ней. Одно и то же у
            // дерева и у постройки — обе оттуда торчат.
            const auto standingAt = [&](float screenX, float screenY) {
                return Rectangle{screenX, screenY - tileSizeF, tileSizeF, tileSizeF * 2.0f};
            };
            for (int y = firstY; y <= lastY; ++y) {
                for (int x = firstX; x <= lastX; ++x) {
                    const std::size_t cell = static_cast<std::size_t>(y) * snapshot.areaWidth + x;
                    const float screenX = static_cast<float>(x) * tileSizeF - viewX;
                    const float screenY = static_cast<float>(y) * tileSizeF - viewY + kHudHeight;

                    // Трава и куст — под всем остальным: они лежат на земле,
                    // а не на том, что на ней стоит. Растение на клетке одно
                    // (где куст, там нет травы), поэтому и ветка одна.
                    if (drawBushes && snapshot.bushSpeciesAt[cell] >= 0) {
                        const int stage = PlantSprites::stageOf(snapshot.plantGrowth[cell]);
                        const Rectangle at{screenX, screenY, tileSizeF, tileSizeF};
                        const int frame = PlantSprites::frameOf(x, y, now);
                        DrawTexturePro(PlantSprites::bushAtlas(bushDetail),
                                       PlantSprites::bush(bushDetail, snapshot.bushSpeciesAt[cell],
                                                          stage, frame),
                                       at, Vector2{0, 0}, 0.0f, WHITE);
                        // Ягоды поверх куста и не качаются вместе с ним: они
                        // висят на ветках, а качается верх кома.
                        if (cell < snapshot.berries.size() && snapshot.berries[cell] > 0) {
                            DrawTexturePro(PlantSprites::bushAtlas(bushDetail),
                                           PlantSprites::berries(bushDetail, snapshot.berries[cell]),
                                           at, Vector2{0, 0}, 0.0f, WHITE);
                        }
                    } else if (drawGrass && snapshot.plantSpeciesAt[cell] >= 0) {
                        const int stage = PlantSprites::stageOf(snapshot.plantGrowth[cell]);
                        DrawTexturePro(PlantSprites::grassAtlas(grassDetail),
                                       PlantSprites::grass(grassDetail, snapshot.plantSpeciesAt[cell],
                                                           stage, PlantSprites::variantOf(x, y),
                                                           PlantSprites::frameOf(x, y, now)),
                                       Rectangle{screenX, screenY, tileSizeF, tileSizeF}, Vector2{0, 0},
                                       0.0f, WHITE);
                    }

                    // Постройки — снизу вверх в том же порядке, в каком они
                    // лежат на самом деле: подстилка на земле, замысел и
                    // принесённый материал рядом с ней, навес над всем этим.
                    // Тот же порядок, что и у оттенков в текстуре карты
                    // (MapTexture), и это не совпадение: сверху карты и с
                    // высоты глаза видно одно и то же место.
                    if (drawBuildings) {
                        if (snapshot.bedding[cell] > 0.0f) {
                            DrawTexturePro(BuildSprites::atlas(),
                                           BuildSprites::bedding(BuildSprites::stageOf(snapshot.bedding[cell])),
                                           standingAt(screenX, screenY), Vector2{0, 0}, 0.0f, WHITE);
                        }
                        if (snapshot.site[cell] > 0) {
                            DrawTexturePro(BuildSprites::atlas(), BuildSprites::site(snapshot.site[cell]),
                                           standingAt(screenX, screenY), Vector2{0, 0}, 0.0f, WHITE);
                            // Куча принесённого — только на площадке: в
                            // готовой постройке материала не лежит, он в неё
                            // и ушёл. Пустая площадка от полной отличается
                            // именно этим, и по карте должно быть видно, чего
                            // стройке не хватает — рук или веток.
                            if (cell < snapshot.siteMaterial.size() && snapshot.siteMaterial[cell] > 0) {
                                DrawTexturePro(BuildSprites::atlas(), BuildSprites::material(),
                                               standingAt(screenX, screenY), Vector2{0, 0}, 0.0f, WHITE);
                            }
                        }
                        if (snapshot.canopy[cell] > 0.0f) {
                            DrawTexturePro(BuildSprites::atlas(),
                                           BuildSprites::canopy(BuildSprites::stageOf(snapshot.canopy[cell])),
                                           standingAt(screenX, screenY), Vector2{0, 0}, 0.0f, WHITE);
                        }
                    }

                    if (!drawTrees || cell >= snapshot.treeSpeciesAt.size() || snapshot.treeSpeciesAt[cell] < 0) {
                        continue;
                    }
                    const int species = snapshot.treeSpeciesAt[cell];
                    const float growth = snapshot.plantGrowth[cell];
                    if (drawSprites) {
                        DrawTexturePro(TreeSprites::atlas(treeDetail),
                                       TreeSprites::source(treeDetail, species,
                                                           TreeSprites::stageOf(growth),
                                                           TreeSprites::variantOf(x, y),
                                                           TreeSprites::frameOf(x, y, now)),
                                       standingAt(screenX, screenY), Vector2{0, 0}, 0.0f, WHITE);
                    } else {
                        DrawRectangle(static_cast<int>(screenX) + (tileSize - trunkWidth) / 2,
                                      static_cast<int>(screenY) - tileSize, trunkWidth, trunkHeight,
                                      TileColors::tree(species, growth));
                    }
                }
            }
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
            // Хищник рисуется волком, травоядное осталось значком, и это не
            // недоделка. О звере, который ест траву, карта отвечает на один
            // вопрос — сколько его и какой он величины, — и значок отвечает на
            // него лучше рисунка, потому что честно вырождается в точку на
            // отдалённой карте. О хищнике спрашивают другое: где он и не
            // крадётся ли он сейчас, — и на это кружок не отвечал ничем.
            const auto wolfDetail = WolfSprites::detailFor(tileSizeF);
            const auto deerDetail = DeerSprites::detailFor(tileSizeF);
            const bool drawWolves = tileSize >= 6 && WolfSprites::ready(wolfDetail);
            const bool drawDeer = tileSize >= 6 && DeerSprites::ready(deerDetail);
            for (const auto& animal : snapshot.animals) {
                const float screenX = static_cast<float>(animal.x) * tileSizeF - viewX;
                const float screenY = static_cast<float>(animal.y) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                const Color color = animal.predator ? TileColors::predatorSpecies(animal.species)
                                                     : TileColors::herbivoreSpecies(animal.species);
                // Размер значка — от величины тела: и от развитости
                // (детёныш мельче взрослого), и от размера вида (заяц мельче
                // лося). Второе и есть то, ради чего размер заводился: пока
                // все взрослые рисовались одинаковой точкой, сорок мелких и
                // десять крупных выглядели одинаково, а весят они разное.
                //
                // Корень, а не сам размер: значок — это площадь, и зверь
                // втрое тяжелее должен занимать втрое больше места, а не быть
                // втрое шире. Иначе крупный вид накрывает собой три клетки и
                // прячет под собой всё, что там есть.
                const float bodyScale = std::sqrt(std::max(0.05f, animal.adultSize));
                const float scale = animal.predator ? 0.30f : 0.22f;
                const float radius =
                    std::max(1.0f, tileSizeF * (scale + 0.16f * animal.growth) * bodyScale);
                const float centerX = screenX + tileSizeF * 0.5f;
                const float centerY = screenY + tileSizeF * 0.5f;
                const bool asSprite = animal.predator ? drawWolves : drawDeer;
                if (asSprite) {
                    // Величина тела остаётся и у рисунка: ради неё
                    // adult_size и заводился — десять крупных читаются как
                    // фауна, сорок одинаковых мелких как насекомые. Взрослый
                    // зверь обычного размера занимает ровно клетку, крупный
                    // вид выходит за неё.
                    const float side = tileSizeF * bodyScale * (0.80f + 0.20f * animal.growth);
                    // Низом на нижний край клетки, а не по центру: зверь
                    // стоит на земле, как и гоблин, и тень у него нарисована
                    // под лапами.
                    const Rectangle at{centerX - side * 0.5f, screenY + tileSizeF - side, side, side};
                    // Память шага у хищника и у травоядного разной длины, и
                    // спрашивается она у своего модуля: волк быстр, олень нет,
                    // и один порог на обоих врал бы про одного из них.
                    if (animal.predator) {
                        const bool walking = WolfSprites::walkingNow(animal.stepTick, snapshot.tick);
                        DrawTexturePro(WolfSprites::atlas(wolfDetail),
                                       WolfSprites::source(wolfDetail, animal.species,
                                                           WolfSprites::poseOf(animal.desire, walking),
                                                           WolfSprites::stageOf(animal.growth),
                                                           WolfSprites::frameOf(animal.id, snapshot.tick),
                                                           animal.facing),
                                       at, Vector2{0, 0}, 0.0f, WHITE);
                    } else {
                        const bool walking = DeerSprites::walkingNow(animal.stepTick, snapshot.tick);
                        DrawTexturePro(DeerSprites::atlas(deerDetail),
                                       DeerSprites::source(deerDetail, animal.species,
                                                           DeerSprites::poseOf(animal.desire, walking),
                                                           DeerSprites::kindOf(animal.growth, animal.sex),
                                                           DeerSprites::frameOf(animal.id, snapshot.tick),
                                                           animal.facing),
                                       at, Vector2{0, 0}, 0.0f, WHITE);
                    }
                } else if (animal.sex == "male") {
                    DrawRectangle(static_cast<int>(centerX - radius), static_cast<int>(centerY - radius),
                                  std::max(1, static_cast<int>(radius * 2.0f)),
                                  std::max(1, static_cast<int>(radius * 2.0f)), color);
                } else {
                    DrawCircle(static_cast<int>(centerX), static_cast<int>(centerY), radius, color);
                }
                // Тёмная обводка — чтобы светлое животное не терялось на
                // светлой почве; только когда тайл достаточно крупный,
                // иначе она съест сам значок. Рисунку она не нужна: под зверем
                // нарисована своя тень, и обводка обвела бы не зверя, а
                // клетку, в которой он стоит.
                if (tileSizeF >= 8.0f && !asSprite) {
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

        // Гоблины — поверх той же карты и тем же способом, что дерево и
        // постройка: рисунком (GoblinSprites). Он отвечает не на вопрос
        // "где гоблин" — на него отвечал и ромб, — а на те, ради которых на
        // гоблина смотрят: чем занят, куда идёт, вырос ли. Двадцать
        // одинаковых ромбов у ручья и двадцать пьющих — это два разных
        // ответа, и второй виден без единого щелчка по карте.
        //
        // Рисунок — те же шестнадцать пикселей на клетку, что у дерева, и
        // мельче шести от него остаётся каша: тогда гоблин снова рисуется
        // ромбом, той самой фигурой, какой он рисовался до всяких кадров.
        // Это не запасной путь на случай беды, а тот же гоблин, изображённый
        // настолько подробно, насколько его видно. Ромб отличает его от
        // зверя, не полагаясь на цвет (кружок и квадрат уже заняты полом
        // животного, а на мелком тайле цвета сливаются), а размер внутри
        // ромба говорит про пол и рост.
        if (showGoblins) {
            // Подробность рисунка — по величине клетки: вплотную берётся
            // крупный лист, издали мелкий (GoblinSprites::detailFor).
            const GoblinSprites::Detail detail = GoblinSprites::detailFor(tileSizeF);
            const bool drawSprites = tileSize >= 6 && GoblinSprites::ready(detail);
            for (const auto& goblin : snapshot.goblins) {
                const float screenX = static_cast<float>(goblin.x) * tileSizeF - viewX;
                const float screenY = static_cast<float>(goblin.y) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                const float centerX = screenX + tileSizeF * 0.5f;
                const float centerY = screenY + tileSizeF * 0.5f;
                if (drawSprites) {
                    // Занятие складывается из трёх вещей, и ни одной из них
                    // по отдельности не хватает: желание говорит, зачем он
                    // тут, шаг — делает он это или ещё идёт, руки — налегке
                    // или с охапкой (см. GoblinSprites::poseOf).
                    //
                    // Еда и материал в руках здесь одно и то же: со стороны
                    // видна охапка, а не то, из чего она набрана.
                    const bool walking = GoblinSprites::walkingNow(goblin.stepTick, snapshot.tick);
                    const bool loaded = goblin.carried > 0.0f || goblin.material > 0.0f;
                    DrawTexturePro(
                        GoblinSprites::atlas(detail),
                        GoblinSprites::source(detail, goblin.tribe,
                                              GoblinSprites::poseOf(goblin.desire, walking, loaded),
                                              GoblinSprites::stageOf(goblin.growth),
                                              GoblinSprites::frameOf(goblin.id, snapshot.tick),
                                              GoblinSprites::Facing{goblin.facing, goblin.verticalStep}),
                        Rectangle{screenX, screenY, tileSizeF, tileSizeF}, Vector2{0, 0}, 0.0f, WHITE);
                } else {
                    const Color color = TileColors::goblinTribe(goblin.tribe);
                    const float scale = goblin.sex == "male" ? 0.32f : 0.26f;
                    const float radius = std::max(1.5f, tileSizeF * (scale + 0.16f * goblin.growth));
                    const Vector2 top{centerX, centerY - radius};
                    const Vector2 right{centerX + radius, centerY};
                    const Vector2 bottom{centerX, centerY + radius};
                    const Vector2 left{centerX - radius, centerY};
                    // Два треугольника, а не DrawPoly: у raylib многоугольник
                    // рисуется от угла поворота, и ромб из него выходит косым.
                    DrawTriangle(top, left, right, color);
                    DrawTriangle(left, bottom, right, color);
                    if (tileSizeF >= 8.0f) {
                        DrawTriangleLines(top, left, right, Color{20, 34, 32, 200});
                        DrawTriangleLines(left, bottom, right, Color{20, 34, 32, 200});
                    }
                }
                // Раненого видно так же, как и зверя, и одинаково при любой
                // подробности рисунка: полоска — не часть облика гоблина, а
                // отметка поверх него.
                if (goblin.health < 0.99f && tileSizeF >= 6.0f) {
                    const float barWidth = tileSizeF * 0.7f;
                    DrawRectangle(static_cast<int>(centerX - barWidth * 0.5f), static_cast<int>(screenY + 1.0f),
                                  std::max(1, static_cast<int>(barWidth * goblin.health)), 2,
                                  Color{220, 60, 50, 230});
                }
            }
        }

        // Что наблюдаемый гоблин ПОМНИТ — поверх всего остального, кольцами
        // на запомненных клетках. Поверх, а не под: вспомненное место обычно
        // лежит за пределами видимости, там на карте не нарисовано ничего, и
        // заслонять кольцу нечего. Это ответ на вопрос "почему он пошёл
        // туда, где ничего не видно".
        //
        // Цвет говорит, чем место было хорошо, толщина — насколько твёрдо
        // помнится: выцветающее кольцо и есть забывание (core/Knowledge.hpp).
        if (snapshot.watched.kind == "goblin" && !snapshot.watched.knows.empty()) {
            for (const auto& known : snapshot.watched.knows) {
                const float screenX = static_cast<float>(known.x) * tileSizeF - viewX;
                const float screenY = static_cast<float>(known.y) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                Color color{200, 200, 200, 255};
                if (known.kind == "food") {
                    color = Color{120, 200, 90, 255};
                } else if (known.kind == "water") {
                    color = Color{90, 160, 230, 255};
                } else if (known.kind == "rest") {
                    color = Color{225, 190, 100, 255};
                } else if (known.kind == "work") {
                    // Тот же холодный цвет, каким помечена площадка и на
                    // карте, и на рисунке (TileColors::site, BuildSprites):
                    // недоделанное место должно узнаваться одним цветом
                    // всюду, где о нём заходит речь.
                    color = Color{130, 165, 190, 255};
                }
                color.a = static_cast<unsigned char>(70 + std::clamp(known.strength, 0, 100) * 170 / 100);
                const float centerX = screenX + tileSizeF * 0.5f;
                const float centerY = screenY + tileSizeF * 0.5f;
                DrawCircleLines(static_cast<int>(centerX), static_cast<int>(centerY), tileSizeF * 0.45f, color);
                DrawCircleLines(static_cast<int>(centerX), static_cast<int>(centerY), tileSizeF * 0.45f - 1.0f,
                                color);
            }
        }

        // Дорога — поверх зверей: она и есть то, ради чего смотрят. Линия
        // от самого зверя через клетки найденной дороги до цели, а на цели
        // — кольцо. Цвет говорит, за чем он идёт: за живой добычей, к туше
        // или к паре. Дороги нет вовсе — не нарисовано ничего, и это тоже
        // ответ: значит, идти ему не к кому.
        if (watchingWalker && !snapshot.watched.road.empty()) {
            // "teeth" — та же живая добыча, просто уже под зубами.
            const bool huntingPrey =
                snapshot.watched.roadKind == "prey" || snapshot.watched.roadKind == "teeth";
            // "call" — та же пара, но не увиденная, а услышанная (см.
            // hearCall): бледнее и синéе настоящей "mate", потому что это
            // прямая линия на звук, а не путь по клеткам видимости.
            const Color roadColor = huntingPrey ? Color{255, 120, 90, 235}
                                    : snapshot.watched.roadKind == "mate" ? Color{235, 140, 235, 235}
                                    : snapshot.watched.roadKind == "call" ? Color{170, 175, 235, 190}
                                                                           : Color{235, 225, 170, 225};
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
        if (const auto mark = markOf(selection)) {
            const float screenX = static_cast<float>(mark->first) * tileSizeF - viewX;
            const float screenY = static_cast<float>(mark->second) * tileSizeF - viewY + kHudHeight;
            const Color selectionColor{255, 255, 255, 235};
            if (selection.kind == InfoPanel::Target::Kind::Animal) {
                // Кольцо, а не рамка: на клетке может стоять ещё десяток
                // зверей, и рамка вокруг всей клетки не показала бы, за
                // кем именно следят.
                DrawCircleLines(static_cast<int>(screenX + tileSizeF * 0.5f),
                                 static_cast<int>(screenY + tileSizeF * 0.5f), std::max(4.0f, tileSizeF * 0.55f),
                                 selectionColor);
            } else {
                DrawRectangleLinesEx(Rectangle{screenX, screenY, tileSizeF, tileSizeF}, 2.0f, selectionColor);
            }
        }

        // Карточка выбранного — поверх всей карты и последней из того, что на
        // ней лежит: она про выбранное, а не про клетку под собой, и
        // заслонять её нечему.
        SelectionCard::draw(snapshot, selection, cardBounds);

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
    int hudExtraX = 10 + MeasureText(hudLabel.c_str(), 16) + 20;
    if (snapshot.paused) {
        DrawText("PAUSED", hudExtraX, 8, 16, pausedColor);
        hudExtraX += MeasureText("PAUSED", 16) + 20;
    }
    // Отвернувшийся клиент — тут же, рядом с паузой, и по той же причине,
    // по которой надпись о паузе вообще существует: карта застывает, и без
    // слова это неотличимо от остановленного или зависшего мира. Но
    // состояние тут не мира, а взгляда на него — мир продолжает жить.
    if (!network.updatesEnabled()) {
        DrawText("NOT WATCHING (H)", hudExtraX, 8, 16, pausedColor);
    }

    // Верхняя полоса больше не несёт кнопок: Back/Save/Panel/Stop
    // дублировали клавиши (Esc, Ctrl+S, P, Space) и только отнимали место
    // у карты. Список того, что и как включается, — на вкладке Keys.
    //
    // Пока открыт любой из модальных диалогов, панель под ним не должна
    // ловить клики.
    const bool modalOpen = confirmingExit || confirmingRegenerate || waitingToSaveBeforeQuit;
    if (modalOpen) GuiLock();

    // Правая панель. Рисуется после карты и полосы, но до модальных
    // диалогов: она часть экрана, а не наложение поверх него.
    if (panelOpen) {
        DrawRectangleRec(panelBounds, Color{22, 22, 26, 255});

        // Полоса вкладок — своей отрисовкой, а не кнопками raygui: у
        // кнопки нет состояния "выбрана", а именно это и нужно показать.
        const Tab tabs[] = {Tab::Info, Tab::Params, Tab::Graphs, Tab::Genome, Tab::Keys};
        const char* labels[] = {"Info", "Params", "Graphs", "Genome", "Keys"};
        constexpr int kTabCount = 5;
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
                    if (snapshot.generated) {
                        // Мир уже есть и мог не быть сохранён — регенерация
                        // сотрёт его безвозвратно, поэтому сперва спрашиваем,
                        // как и при выходе с экрана. Сами параметры
                        // запоминаем: диалог откроется на следующих кадрах,
                        // а панель к тому времени успеет перерисоваться.
                        pendingRegenerateParams = panelParams;
                        confirmingRegenerate = true;
                    } else {
                        // Мира ещё нет — спрашивать не о чем, терять нечего.
                        // Симуляция при этом стартует сразу: игрок только что
                        // подобрал параметры именно затем, чтобы начать игру,
                        // а не затем, чтобы дальше жать паузу самому.
                        network.sendRegenerate(panelParams);
                        if (snapshot.paused) {
                            network.sendTogglePause();
                        }
                    }
                }
                if (saveParamsRequested) {
                    network.sendSaveGenerationConfig(panelParams);
                }
                break;
            }
            case Tab::Graphs:
                PopulationGraph::draw(snapshot, panelContent, !modalOpen);
                break;
            case Tab::Genome:
                GenomeGraph::draw(snapshot, panelContent, !modalOpen);
                break;
            case Tab::Keys:
                KeysPanel::draw(panelContent, layers, network.updatesEnabled());
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
        // Крестик отменяет любой другой открытый вопрос: закрыться в этот
        // момент важнее, чем спросить про уже начатую регенерацию, а
        // задавать оба вопроса сразу (два наложенных диалога) было бы
        // просто нечитаемо. Повторный крестик, нажатый уже во время
        // ожидания подтверждения сохранения, начинает вопрос заново —
        // мало ли что случилось за это время.
        confirmingRegenerate = false;
        waitingToSaveBeforeQuit = false;
        confirmingExit = true;
        exitingApp = true;
    }

    // Кнопка Back ушла вместе с остальными кнопками полосы — выйти можно
    // только клавишей Esc, и диалог подтверждения открывает именно она.
    // Диалог о регенерации, если он открыт, отменяется той же клавишей
    // первым — у него приоритет перед выходом, а не наоборот.
    const bool escapePressed = IsKeyPressed(KEY_ESCAPE);
    if (confirmingRegenerate) {
        if (escapePressed) {
            confirmingRegenerate = false;
        }
    } else if (!confirmingExit) {
        if (escapePressed) {
            confirmingExit = true;
            exitingApp = false;
        }
    } else if (escapePressed) {
        confirmingExit = false;
        exitingApp = false;
    }

    if (confirmingRegenerate) {
        DrawRectangle(0, 0, screenW, screenH, Color{0, 0, 0, 150});

        const int boxW = 380;
        const int boxH = 130;
        const int boxX = screenW / 2 - boxW / 2;
        const int boxY = screenH / 2 - boxH / 2;
        DrawRectangle(boxX, boxY, boxW, boxH, hudColor);
        DrawRectangleLines(boxX, boxY, boxW, boxH, textColor);
        DrawText("Save world before regenerating?", boxX + 20, boxY + 16, 18, textColor);

        const bool saveRegen = GuiButton(
            Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 56, 160, 30}, "Save & Regenerate");
        const bool discardRegen = GuiButton(
            Rectangle{static_cast<float>(boxX) + 200, static_cast<float>(boxY) + 56, 160, 30}, "Discard & Regenerate");
        const bool cancelRegen = GuiButton(
            Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 94, 340, 26}, "Cancel (Esc)");

        // Как и при выходе: сервер сохраняет между тиками, ответа клиент
        // не ждёт — попросить сохранить, следом перегенерировать.
        if (saveRegen || discardRegen) {
            if (saveRegen) {
                network.sendSaveWorld();
            }
            network.sendRegenerate(pendingRegenerateParams);
            confirmingRegenerate = false;
        }
        if (cancelRegen) {
            confirmingRegenerate = false;
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

        // Мир сохраняет сервер, между тиками, и обычно ответа клиент не
        // ждёт: экран остаётся тем же World (возврат в меню его просто
        // сменит), соединение никуда не девается, и запрос спокойно
        // дойдёт своим чередом. Единственное исключение — "Save & Quit":
        // следом рвётся само соединение (main.cpp, network.disconnect
        // после этого экрана), а send() у IXWebSocket не гарантирует, что
        // байты успели уйти на провод к моменту разрыва. Здесь и только
        // здесь дожидаемся настоящего подтверждения (notice от сервера),
        // прежде чем отдать AppScreen::Exit — иначе "Save & Quit" мог
        // закрыться, ничего не сохранив, хотя и честно спросив.
        if (saveExit && exitingApp) {
            waitingNoticeBaseline = snapshot.noticeAt;
            waitingSince = std::chrono::steady_clock::now();
            waitingToSaveBeforeQuit = true;
            network.sendSaveWorld();
            confirmingExit = false;
            exitingApp = false;
        } else if (saveExit || discardExit) {
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

    // Ждём подтверждения от сервера, что "Save & Quit" действительно
    // сохранил мир (см. комментарий у saveExit && exitingApp выше), прежде
    // чем отдать AppScreen::Exit и позволить main.cpp разорвать
    // соединение. Кнопок здесь нет: решение уже принято, отменять нечего
    // — разве что снова нажать на крестик (см. closeRequested выше).
    if (waitingToSaveBeforeQuit) {
        DrawRectangle(0, 0, screenW, screenH, Color{0, 0, 0, 150});

        const int boxW = 380;
        const int boxH = 90;
        const int boxX = screenW / 2 - boxW / 2;
        const int boxY = screenH / 2 - boxH / 2;
        DrawRectangle(boxX, boxY, boxW, boxH, hudColor);
        DrawRectangleLines(boxX, boxY, boxW, boxH, textColor);
        const char* label = "Saving world before quitting...";
        DrawText(label, boxX + boxW / 2 - MeasureText(label, 18) / 2, boxY + boxH / 2 - 9, 18, textColor);

        // Свежий notice после waitingNoticeBaseline — сервер получил и
        // обработал save_world (успешно или нет, неважно: важен сам
        // факт, что запрос пережил дорогу и не потерялся при разрыве
        // соединения следом). Таймаут — на случай, если сервер не
        // отвечает вовсе (упал, завис, ушла сеть): дожидаться ответа
        // бесконечно нельзя, приложение должно закрываться и без сервера.
        constexpr auto kSaveConfirmTimeout = std::chrono::seconds(3);
        const bool confirmed = snapshot.noticeAt > waitingNoticeBaseline;
        const bool timedOut = std::chrono::steady_clock::now() - waitingSince > kSaveConfirmTimeout;
        if (confirmed || timedOut) {
            waitingToSaveBeforeQuit = false;
            network.sendStopSimulation();
            return AppScreen::Exit;
        }
        // Иначе просто ждём дальше — падаем к оверлею констант ниже, как
        // и диалоги выше: он рисуется поверх всего, включая этот экран.
    }

    // Оверлей констант — последним: он перекрывает всё, включая диалог выхода.
    ConstantsOverlay::update(snapshot);

    return AppScreen::World;
}

} // namespace WorldScreen

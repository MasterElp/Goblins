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
// Ширина панели генерации, когда она открыта. То же число, что было на
// отдельном экране генерации — под ним подобраны ширины ползунков.
constexpr float kPanelWidth = 460.0f;
// Полоса подсказки по клавишам вдоль нижнего края — под ней ничего не
// рисуется, поэтому график популяции упирается именно в неё.
constexpr float kHintHeight = 26.0f;

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
    static bool showHerbivores = true;
    static bool panelOpen = false;
    static bool showGraph = false;
    if (!initialized) {
        zoom = config.zoom;
        showRockiness = config.show_rockiness;
        showCompaction = config.show_compaction;
        showMoisture = config.show_moisture;
        showMinerals = config.show_minerals;
        showHeight = config.show_height;
        showPlants = config.show_plants;
        showHerbivores = config.show_herbivores;
        panelOpen = config.show_generation_panel;
        showGraph = config.show_population_graph;
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
    const float panelX = static_cast<float>(screenW) - kPanelWidth;
    const int viewportW = panelOpen ? std::max(1, static_cast<int>(panelX)) : screenW;
    const int viewportH = screenH - kHudHeight;

    // График популяции — полосой по нижнему краю карты, а не оверлеем во
    // весь экран, как константы: кривая нужна рядом с картой (просело
    // стадо — тут же видно, где именно объеден луг). Высота от окна, но с
    // потолком и полом: на низком окне график съел бы карту, на высоком —
    // растянулся бы без пользы.
    const float graphHeight = std::clamp(static_cast<float>(screenH) * 0.30f, 150.0f, 260.0f);
    const Rectangle graphBounds{0.0f, static_cast<float>(screenH) - kHintHeight - graphHeight,
                                static_cast<float>(viewportW), graphHeight};

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
    // Колесо над панелью настроек прокручивает её саму (GuiScrollPanel), а
    // не меняет масштаб карты — иначе одно движение колеса делало бы сразу
    // два несвязанных действия. Полоса графика популяции — по той же
    // причине не карта: под ней курсор читает точку истории, а не тайл, и
    // подсвечивать сквозь неё клетку (и уж тем более зумить) незачем.
    const bool mouseOverMap = mouse.x < static_cast<float>(viewportW) &&
                              !(showGraph && CheckCollisionPointRec(mouse, graphBounds));

    // Пока открыт диалог подтверждения выхода или диалог сохранения, мир
    // под ним не должен реагировать на ввод (прокрутка/зум/слои/пауза) —
    // иначе клик по кнопке диалога совпадёт с движением камеры или сменой
    // слоя позади, а буквенные клавиши при вводе имени — с WASD/P/1-6.
    // Оверлей констант — по той же причине: он перекрывает экран целиком,
    // и слои под ним переключались бы вслепую.
    if (!confirmingExit && !showSaveDialog && !ConstantsOverlay::visible()) {
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

        // Панель генерации — сворачиваемая: 460px справа нужны, только
        // когда параметры действительно крутят, а смотреть на идущую
        // симуляцию удобнее во всё окно.
        if (IsKeyPressed(KEY_G)) {
            panelOpen = !panelOpen;
            config.show_generation_panel = panelOpen;
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
        // Травоядные — отдельный выключатель, а не часть слоя травы: это
        // не слой почвы, а объекты поверх карты (как булыжники и
        // источники), и смотреть на луг без стада — обычное дело.
        if (IsKeyPressed(KEY_SEVEN)) {
            showHerbivores = !showHerbivores;
            config.show_herbivores = showHerbivores;
            goblins::saveClientConfig(configPath, config);
        }

        // График численности видов по времени. Отдельная клавиша, а не
        // слой карты: слои 1-7 отвечают на вопрос "что сейчас на этой
        // клетке", а график — "что было со всем миром до сих пор", и
        // держать их в одном ряду значило бы путать эти два вопроса.
        // Летопись при этом ведёт сервер, а не этот экран, — она приходит
        // вместе с состоянием мира и не зависит от того, показан ли
        // график и открыт ли вообще клиент.
        if (IsKeyPressed(KEY_H)) {
            showGraph = !showGraph;
            config.show_population_graph = showGraph;
            goblins::saveClientConfig(configPath, config);
        }

        // Пауза — не локальное состояние клиента, а запрос серверу (настоящая
        // пауза мира). Сам клиент своё "paused" не выставляет — ждёт
        // подтверждения через pause_state/world_delta, чтобы все
        // подключённые клиенты видели одно и то же состояние.
        if (IsKeyPressed(KEY_P)) {
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

        // Травоядные — поверх карты, а не в её текстуре: они не свойство
        // тайла, их может быть несколько на одной клетке, и меняются они
        // каждый тик (пересобирать из-за них всю текстуру мира было бы
        // дороже всего остального вместе взятого). Самка — кружок, самец —
        // квадрат: пол виден сразу, а на сильно отдалённой карте и то, и
        // другое честно вырождается в точку.
        if (showHerbivores) {
            for (const auto& animal : snapshot.herbivores) {
                const float screenX = static_cast<float>(animal.x) * tileSizeF - viewX;
                const float screenY = static_cast<float>(animal.y) * tileSizeF - viewY + kHudHeight;
                if (screenX + tileSize < 0 || screenX > viewportW || screenY + tileSize < kHudHeight ||
                    screenY > viewportH + kHudHeight) {
                    continue;
                }
                const Color color = TileColors::herbivoreSpecies(animal.species);
                // Размер значка — от развитости: телёнок мельче взрослого,
                // как и на самом деле.
                const float radius = std::max(1.0f, tileSizeF * (0.22f + 0.16f * animal.growth));
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
            }
        }

        if (hasHoverTile) {
            const float screenX = static_cast<float>(hoverX) * tileSizeF - viewX;
            const float screenY = static_cast<float>(hoverY) * tileSizeF - viewY + kHudHeight;
            DrawRectangleLines(static_cast<int>(screenX), static_cast<int>(screenY), tileSize, tileSize, cursorColor);
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
        drawLayerLabel("[7] Herbivores", showHerbivores);

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

        // Стадо: сколько особей каждого вида и чем они сейчас заняты.
        // Желания в легенде не для красоты — по ним видно состояние мира
        // целиком: стадо, поголовно ищущее еду, означает объеденный луг.
        if (showHerbivores && !snapshot.herbivores.empty()) {
            std::vector<int> population(std::max<std::size_t>(snapshot.herbivoreSpecies.size(), 1), 0);
            std::map<std::string, int> desires;
            for (const auto& animal : snapshot.herbivores) {
                if (animal.species >= 0 && static_cast<std::size_t>(animal.species) < population.size()) {
                    ++population[static_cast<std::size_t>(animal.species)];
                }
                ++desires[animal.desire];
            }

            int legendX = 10;
            const int legendY = layerY + 40;
            for (std::size_t s = 0; s < population.size(); ++s) {
                DrawRectangle(legendX, legendY + 1, 10, 10, TileColors::herbivoreSpecies(static_cast<int>(s)));
                const char* label = TextFormat("hb%zu %d", s, population[s]);
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
    buttonX -= 130.0f;
    const bool panelPressed =
        GuiButton(Rectangle{buttonX, 2, 120, kHudHeight - 4}, panelOpen ? "Hide params (G)" : "Params (G)");
    buttonX -= 80.0f;
    const bool pausePressed = GuiButton(Rectangle{buttonX, 2, 70, kHudHeight - 4}, snapshot.paused ? "Start" : "Stop");

    if (savePressed) {
        std::snprintf(saveNameBuffer, sizeof(saveNameBuffer), "%s", snapshot.currentWorld.c_str());
        showSaveDialog = true;
    }
    if (panelPressed) {
        panelOpen = !panelOpen;
        config.show_generation_panel = panelOpen;
        goblins::saveClientConfig(configPath, config);
    }
    if (pausePressed) {
        network.sendTogglePause();
    }

    // Панель генерации. Рисуется после карты и полосы, но до модальных
    // диалогов: она часть экрана, а не наложение поверх него.
    if (panelOpen) {
        goblins::RegenerationRequest panelParams;
        bool saveParamsRequested = false;
        const Rectangle panelBounds{panelX, 0, kPanelWidth, static_cast<float>(screenH)};
        if (panel.draw(panelBounds, panelParams, saveParamsRequested)) {
            network.sendRegenerate(panelParams);
        }
        if (saveParamsRequested) {
            network.sendSaveGenerationConfig(panelParams);
        }
    }

    if (modalOpen) GuiUnlock();

    // График — после карты и панели, но до подписей по нижнему краю: они
    // должны ложиться поверх него (см. bottomInfoY ниже), а не под него.
    // Чтение значений под курсором отключается вместе с остальным вводом,
    // пока открыт модальный диалог.
    if (showGraph) {
        PopulationGraph::draw(snapshot, graphBounds, !modalOpen);
    }

    // Подсказка по клавишам — по нижнему краю слева, мелким приглушённым
    // текстом: в верхней полосе её теснят имя мира и кнопки, а нужна она
    // редко.
    DrawText("WASD-scroll  Wheel-zoom  F-fit  1-7-layers  P-pause  G-params  H-graphs  C-constants  Esc-menu", 10,
             screenH - 20, 14, mutedColor);

    // Строка с параметрами тайла под курсором (и сообщение сервера рядом с
    // ней) — над графиком, когда он открыт: иначе они оказались бы под
    // его подложкой и читались бы только по краям.
    const int bottomInfoY = showGraph ? static_cast<int>(graphBounds.y) - 22 : screenH - 42;

    // Параметры тайла под курсором — по нижнему краю справа (над панелью
    // настроек, если она открыта, места нет — поэтому от края карты).
    if (hasHoverTile) {
        const std::size_t hi = static_cast<std::size_t>(hoverY) * snapshot.areaWidth + hoverX;
        const bool hoverIsSource =
            std::any_of(snapshot.waterSources.begin(), snapshot.waterSources.end(),
                        [&](const auto& s) { return s.first == hoverX && s.second == hoverY; });
        const std::string tileLabel =
            TextFormat("Tile (%d,%d)  moist %.2f  rock %.2f  pack %.2f  min %d%s%s%s%s", hoverX, hoverY,
                       snapshot.moisture[hi], snapshot.rockiness[hi], snapshot.compaction[hi], snapshot.minerals[hi],
                       snapshot.waterDepth[hi] > 0.0f ? TextFormat("  water %.2f", snapshot.waterDepth[hi]) : "",
                       hoverIsSource ? "  [source]" : "",
                       snapshot.plantSpeciesAt[hi] >= 0
                           ? TextFormat("  grass sp%d %.0f%%", snapshot.plantSpeciesAt[hi],
                                        snapshot.plantGrowth[hi] * 100.0f)
                           : "",
                       snapshot.humus[hi] > 0 ? TextFormat("  humus %d", snapshot.humus[hi]) : "");
        const int labelWidth = MeasureText(tileLabel.c_str(), 16);
        DrawText(tileLabel.c_str(), viewportW - labelWidth - 12, bottomInfoY, 16, cursorColor);

        // Животные под курсором — отдельной строкой над строкой тайла: их
        // на клетке может быть несколько, и втискивать их в подпись самого
        // тайла (у которого всё по одному) значило бы врать о том, как
        // устроен мир.
        if (showHerbivores) {
            std::string animalsLabel;
            for (const auto& animal : snapshot.herbivores) {
                if (animal.x != hoverX || animal.y != hoverY) {
                    continue;
                }
                if (!animalsLabel.empty()) {
                    animalsLabel += "   ";
                }
                animalsLabel += TextFormat("hb%d %s %.0f%% -> %s", animal.species, animal.sex.c_str(),
                                            animal.growth * 100.0f, animal.desire.c_str());
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

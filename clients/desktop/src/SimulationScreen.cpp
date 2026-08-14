#include "SimulationScreen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>
#include <raygui.h>
#include <raylib.h>

#include "MapTexture.hpp"
#include "TileColors.hpp"

namespace SimulationScreen {

namespace {
constexpr int kHudHeight = 32;
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 4.0f;
constexpr float kZoomStep = 1.1f;
} // namespace

AppScreen draw(NetworkClient& network, const goblins::ClientConfig& config) {
    // Персистентны между кадрами, пока это состояние активно (при
    // возврате в меню и обратно позиция прокрутки сохраняется — это
    // осознанное поведение, не забытый сброс).
    static float viewX = 0.0f;
    static float viewY = 0.0f;
    static float zoom = 1.0f;
    static bool showRockiness = true;
    static bool showCompaction = true;
    static bool showMoisture = true;
    static bool showMinerals = true;
    static bool showHeight = true;
    static bool showPlants = true;
    static bool confirmingExit = false;
    // Диалог ввода имени при сохранении — открывается кнопкой "Save
    // world", буфер предзаполняется именем текущего мира (пусто, если
    // мир ещё не сохранён).
    static bool showSaveDialog = false;
    static char saveNameBuffer[64] = "";
    // Карта в текстуре — тоже состояние экрана: пересобирается только
    // когда пришло новое состояние мира или переключён слой.
    static MapTexture::Cache mapCache;

    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();
    const int viewportW = screenW;
    const int viewportH = screenH - kHudHeight;

    const Color backgroundColor{28, 28, 32, 255};
    const Color boulderColor{70, 66, 62, 255};
    const Color hudColor{18, 18, 20, 255};
    const Color textColor{230, 230, 230, 255};
    const Color cursorColor{255, 220, 90, 255};
    const Color pausedColor{220, 70, 70, 255};

    // Масштаб — рантайм-множитель поверх config.tile_size (базового
    // размера тайла), не персистится: при перезапуске клиента сбрасывается
    // к 100%, как и позиция прокрутки.
    float tileSizeF = static_cast<float>(config.tile_size) * zoom;

    const float dt = GetFrameTime();
    const float scrollSpeedPx = static_cast<float>(config.scroll_step) * tileSizeF;

    // Пока открыт диалог подтверждения выхода или диалог сохранения, мир
    // под ним не должен реагировать на ввод (прокрутка/зум/слои/пауза) —
    // иначе клик по кнопке диалога совпадёт с движением камеры или сменой
    // слоя позади, а буквенные клавиши при вводе имени — с WASD/P/1-6.
    if (!confirmingExit && !showSaveDialog) {
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) viewY -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) viewY += scrollSpeedPx * dt;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) viewX -= scrollSpeedPx * dt;
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) viewX += scrollSpeedPx * dt;

        // Зум колёсиком мыши — к точке под курсором: мировая координата под
        // курсором остаётся на месте, чтобы приближение/отдаление не
        // "убегало" в сторону.
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            const Vector2 wheelMouse = GetMousePosition();
            const float worldX = wheelMouse.x + viewX;
            const float worldY = wheelMouse.y - kHudHeight + viewY;

            const float factor = wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep;
            zoom = std::clamp(zoom * factor, kMinZoom, kMaxZoom);
            tileSizeF = static_cast<float>(config.tile_size) * zoom;

            viewX = worldX * factor - wheelMouse.x;
            viewY = worldY * factor - wheelMouse.y;
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
        if (IsKeyPressed(KEY_ONE)) showRockiness = !showRockiness;
        if (IsKeyPressed(KEY_TWO)) showCompaction = !showCompaction;
        if (IsKeyPressed(KEY_THREE)) showMoisture = !showMoisture;
        if (IsKeyPressed(KEY_FOUR)) showMinerals = !showMinerals;
        if (IsKeyPressed(KEY_FIVE)) showHeight = !showHeight;
        // Растения и перегной — один выключатель (KEY_SIX): перегной это
        // и есть след умершего растения, разделять их значило бы видеть
        // остатки при погашенном слое травы.
        if (IsKeyPressed(KEY_SIX)) showPlants = !showPlants;

        // Пауза — не локальное состояние клиента, а запрос серверу (настоящая
        // пауза мира). Сам клиент своё "paused" не выставляет — ждёт
        // подтверждения через pause_state/world_snapshot, чтобы все
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

    const Vector2 mouse = GetMousePosition();
    bool hasHoverTile = false;
    int hoverX = 0;
    int hoverY = 0;
    if (snapshot.areaWidth > 0 && mouse.x >= 0 && mouse.x < viewportW && mouse.y >= kHudHeight &&
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
            DrawRectangle(static_cast<int>(screenX) + 2, static_cast<int>(screenY) + 2, tileSize - 4, tileSize - 4,
                          boulderColor);
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
    }

    DrawRectangle(0, 0, viewportW, kHudHeight, hudColor);
    DrawText(TextFormat("%s   Tick: %llu   Area: %dx%d   View: (%d,%d)   Zoom: %d%%   WASD-scroll  Wheel-zoom  "
                         "1-6-layers  P-pause  Esc-menu",
                         snapshot.currentWorld.empty() ? "(unsaved world)" : snapshot.currentWorld.c_str(),
                         static_cast<unsigned long long>(snapshot.tick), snapshot.areaWidth, snapshot.areaHeight,
                         static_cast<int>(viewX / tileSizeF), static_cast<int>(viewY / tileSizeF),
                         static_cast<int>(std::round(zoom * 100.0f))),
             10, 8, 16, textColor);

    // Сохранение — состояние мира целиком на текущем тике, поверх того же
    // файла, из которого мир загружен (а если он ещё безымянный — в
    // новый). Сам мир при этом не останавливается: сервер сохраняет между
    // тиками. Кнопка открывает диалог с именем вместо немедленного
    // сохранения — так можно и сохранить под текущим именем (просто
    // нажать Save), и сделать копию мира под новым.
    if (confirmingExit || showSaveDialog) GuiLock();
    const bool savePressed =
        GuiButton(Rectangle{static_cast<float>(screenW) - 220, 2, 100, kHudHeight - 4}, "Save world");
    if (savePressed) {
        std::snprintf(saveNameBuffer, sizeof(saveNameBuffer), "%s", snapshot.currentWorld.c_str());
        showSaveDialog = true;
    }

    bool backPressed = GuiButton(Rectangle{static_cast<float>(screenW) - 110, 2, 100, kHudHeight - 4}, "Back (Esc)");
    if (confirmingExit || showSaveDialog) GuiUnlock();

    // Параметры тайла под курсором — по нижнему краю справа, как на
    // экране генерации: в верхней полосе их теснят имя мира и кнопки.
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
        DrawText(tileLabel.c_str(), screenW - labelWidth - 12, screenH - 24, 16, cursorColor);
    }

    // Результат сохранения (или ошибка загрузки, если мир так и не
    // открылся) — единственное место, где клиент об этом узнаёт.
    if (hasFreshNotice(snapshot)) {
        DrawText(snapshot.notice.c_str(), 10, screenH - 24, 16,
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
    return AppScreen::Simulation;
}

} // namespace SimulationScreen

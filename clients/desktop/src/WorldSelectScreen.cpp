#include "WorldSelectScreen.hpp"

#include <algorithm>
#include <string>

#include <raygui.h>
#include <raylib.h>

namespace WorldSelectScreen {

namespace {

constexpr float kRowHeight = 58.0f;
constexpr float kRowGap = 6.0f;

// Строка списка: имя мира крупно, под ним — чем этот мир отличается от
// соседнего (тик, размер Области, seed, когда сохранён). Сама строка —
// кнопка целиком (текст рисуем поверх), чтобы попадать по ней мышью не
// целясь в маленькую "Load".
bool worldRow(Rectangle bounds, const goblins::WorldSaveInfo& world, bool isCurrent) {
    // Строка рисуется поверх кнопки raygui, а она светлая — цвета текста
    // здесь тёмные, в отличие от остальных экранов, где текст лежит на
    // тёмном фоне окна.
    const Color textColor{30, 30, 34, 255};
    const Color mutedColor{95, 95, 100, 255};
    const Color currentColor{170, 105, 0, 255};

    const bool clicked = GuiButton(bounds, "");

    DrawText(world.name.c_str(), static_cast<int>(bounds.x) + 12, static_cast<int>(bounds.y) + 8, 20,
             isCurrent ? currentColor : textColor);
    DrawText(TextFormat("tick %llu   %dx%d   terrain seed %u   saved %s",
                        static_cast<unsigned long long>(world.tick), world.area_width, world.area_height,
                        world.terrain_seed, world.saved_at.c_str()),
             static_cast<int>(bounds.x) + 12, static_cast<int>(bounds.y) + 34, 14, mutedColor);

    const char* action = world.tick == 0 ? "Play (as generated)" : "Continue";
    const int actionWidth = MeasureText(action, 16);
    DrawText(action, static_cast<int>(bounds.x + bounds.width) - actionWidth - 14, static_cast<int>(bounds.y) + 20, 16,
             mutedColor);

    return clicked;
}

} // namespace

AppScreen draw(NetworkClient& network) {
    // Прокрутка списка персистентна между кадрами, пока экран активен.
    static Vector2 scroll{0, 0};

    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();

    ClearBackground(Color{28, 28, 32, 255});

    const Color textColor{230, 230, 230, 255};
    const Color mutedColor{160, 160, 160, 255};
    const Color errorColor{230, 110, 110, 255};

    DrawText("Select world", 30, 24, 28, textColor);
    DrawText("Simulation always runs a saved world: pick one to continue, or create a new one.", 30, 60, 16,
             mutedColor);

    const bool backPressed = GuiButton(Rectangle{static_cast<float>(screenW) - 140, 24, 110, 32}, "Back (Esc)");
    if (backPressed || IsKeyPressed(KEY_ESCAPE)) {
        return AppScreen::MainMenu;
    }

    const WorldState snapshot = network.snapshot();

    if (!snapshot.connected) {
        DrawText("Connecting to server...", 30, screenH / 2 - 10, 20, mutedColor);
        return AppScreen::WorldSelect;
    }

    if (!snapshot.worldsReceived) {
        DrawText("Loading saved worlds...", 30, screenH / 2 - 10, 20, mutedColor);
        return AppScreen::WorldSelect;
    }

    // Сохранённых миров нет — выбирать не из чего, поэтому сразу просим
    // сервер сгенерировать новый (он же его и сохранит как мир с нулевым
    // тиком) и уходим в симуляцию. Кадром позже этот экран уже не
    // рисуется, так что запрос уходит ровно один раз.
    if (snapshot.worlds.empty()) {
        DrawText("No saved worlds yet - generating a new one...", 30, screenH / 2 - 10, 20, textColor);
        network.sendStartSimulation();
        return AppScreen::Simulation;
    }

    if (GuiButton(Rectangle{30, 92, 220, 34}, "New world")) {
        network.sendStartSimulation();
        return AppScreen::Simulation;
    }
    DrawText("New world uses the parameters from the World Generation screen.", 262, 102, 14, mutedColor);

    const float listY = 140.0f;
    const float listBottom = static_cast<float>(screenH) - 50.0f;
    const Rectangle listBounds{30, listY, static_cast<float>(screenW) - 60, std::max(kRowHeight, listBottom - listY)};

    const float contentHeight = static_cast<float>(snapshot.worlds.size()) * (kRowHeight + kRowGap) + kRowGap;
    Rectangle view{};
    const Rectangle content{0, 0, listBounds.width - 18, contentHeight};
    GuiScrollPanel(listBounds, nullptr, content, &scroll, &view);

    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y), static_cast<int>(view.width),
                      static_cast<int>(view.height));

    std::string selected;
    for (std::size_t i = 0; i < snapshot.worlds.size(); ++i) {
        const Rectangle row{listBounds.x + 8 + scroll.x,
                            listBounds.y + kRowGap + scroll.y + static_cast<float>(i) * (kRowHeight + kRowGap),
                            listBounds.width - 34, kRowHeight};
        // Строки, целиком уехавшие за пределы панели, не рисуем и не
        // опрашиваем: raygui о ножницах не знает и засчитал бы клик по
        // невидимой строке.
        if (row.y + row.height < view.y || row.y > view.y + view.height) {
            continue;
        }
        if (worldRow(row, snapshot.worlds[i], snapshot.worlds[i].name == snapshot.currentWorld)) {
            selected = snapshot.worlds[i].name;
        }
    }

    EndScissorMode();

    if (hasFreshNotice(snapshot)) {
        DrawText(snapshot.notice.c_str(), 30, screenH - 34, 16, snapshot.noticeIsError ? errorColor : mutedColor);
    }

    if (!selected.empty()) {
        network.sendStartSimulation(selected);
        return AppScreen::Simulation;
    }

    return AppScreen::WorldSelect;
}

} // namespace WorldSelectScreen

#include "WorldSelectScreen.hpp"

#include <algorithm>
#include <string>

#include <raygui.h>
#include <raylib.h>

#include "WorldScreen.hpp"

namespace WorldSelectScreen {

namespace {

constexpr float kRowHeight = 58.0f;
constexpr float kRowGap = 6.0f;

// Строка списка: имя мира крупно, под ним — чем этот мир отличается от
// соседнего (тик, размер Области, seed, когда сохранён). Сама строка —
// кнопка целиком (текст рисуем поверх), чтобы попадать по ней мышью не
// целясь в маленькую "Load". Кнопка "Delete" рисуется поверх той же
// строки в отдельном углу — при клике по ней клик по строке-кнопке
// (raygui не знает про z-order ввода) отбрасывается вызывающей стороной.
struct RowClick {
    bool load = false;
    bool remove = false;
};

RowClick worldRow(Rectangle bounds, const goblins::WorldSaveInfo& world, bool isCurrent) {
    // Строка рисуется поверх кнопки raygui, а она светлая — цвета текста
    // здесь тёмные, в отличие от остальных экранов, где текст лежит на
    // тёмном фоне окна.
    const Color textColor{30, 30, 34, 255};
    const Color mutedColor{95, 95, 100, 255};
    const Color currentColor{170, 105, 0, 255};

    const bool rowClicked = GuiButton(bounds, "");

    DrawText(world.name.c_str(), static_cast<int>(bounds.x) + 12, static_cast<int>(bounds.y) + 8, 20,
             isCurrent ? currentColor : textColor);
    DrawText(TextFormat("tick %llu   %dx%d   seed %u   saved %s",
                        static_cast<unsigned long long>(world.tick), world.area_width, world.area_height,
                        world.seed, world.saved_at.c_str()),
             static_cast<int>(bounds.x) + 12, static_cast<int>(bounds.y) + 34, 14, mutedColor);

    const char* action = world.tick == 0 ? "Play (as generated)" : "Continue";
    const int actionWidth = MeasureText(action, 14);
    DrawText(action, static_cast<int>(bounds.x + bounds.width) - actionWidth - 14, static_cast<int>(bounds.y) + 8, 14,
             mutedColor);

    const Rectangle deleteRect{bounds.x + bounds.width - 82, bounds.y + bounds.height - 28, 70, 20};
    const bool deleteClicked = GuiButton(deleteRect, "Delete");

    RowClick result;
    result.remove = deleteClicked;
    result.load = rowClicked && !deleteClicked;
    return result;
}

} // namespace

AppScreen draw(NetworkClient& network) {
    // Прокрутка списка персистентна между кадрами, пока экран активен.
    static Vector2 scroll{0, 0};
    // Имя мира, который сейчас предлагается удалить (диалог
    // подтверждения) — пусто, если диалог закрыт.
    static std::string pendingDelete;

    const int screenW = GetScreenWidth();
    const int screenH = GetScreenHeight();

    ClearBackground(Color{28, 28, 32, 255});

    const Color textColor{230, 230, 230, 255};
    const Color mutedColor{160, 160, 160, 255};
    const Color errorColor{230, 110, 110, 255};

    DrawText("Select world", 30, 24, 28, textColor);
    DrawText("Pick a world to open, or create a new one.", 30, 60, 16, mutedColor);

    // Пока открыт диалог подтверждения удаления, фон не должен реагировать
    // на клики (Back/строки списка) под ним; Esc в этом состоянии
    // закрывает диалог, а не уходит в меню.
    if (!pendingDelete.empty()) GuiLock();
    const bool backPressed = GuiButton(Rectangle{static_cast<float>(screenW) - 140, 24, 110, 32}, "Back (Esc)");
    if (!pendingDelete.empty()) GuiUnlock();
    const bool escapePressed = IsKeyPressed(KEY_ESCAPE);
    if (pendingDelete.empty()) {
        if (backPressed || escapePressed) {
            return AppScreen::MainMenu;
        }
    } else if (escapePressed) {
        pendingDelete.clear();
    }

    const auto statePtr = network.snapshot();
    const WorldState& snapshot = *statePtr;

    if (!snapshot.connected) {
        DrawText("Connecting to server...", 30, screenH / 2 - 10, 20, mutedColor);
        return AppScreen::WorldSelect;
    }

    if (!snapshot.worldsReceived) {
        DrawText("Loading saved worlds...", 30, screenH / 2 - 10, 20, mutedColor);
        return AppScreen::WorldSelect;
    }

    // Сохранённых миров нет — выбирать не из чего, поэтому уходим прямо к
    // параметрам генерации нового. Мир при этом НЕ создаётся сам:
    // размер карты, seed и всё остальное выбирают до создания, а не
    // перегенерацией уже созданного (кнопка "Create world" на панели).
    if (snapshot.worlds.empty()) {
        DrawText("No saved worlds yet - opening generation parameters...", 30, screenH / 2 - 10, 20, textColor);
        WorldScreen::requestParamsTab();
        return AppScreen::World;
    }

    if (!pendingDelete.empty()) GuiLock();
    if (GuiButton(Rectangle{30, 92, 220, 34}, "New world")) {
        WorldScreen::requestParamsTab();
        return AppScreen::World;
    }
    DrawText("New world opens the generation parameters -- pick the map size and press \"Create world\".", 262, 102,
             14, mutedColor);

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
        const RowClick click = worldRow(row, snapshot.worlds[i], snapshot.worlds[i].name == snapshot.currentWorld);
        if (click.load) {
            selected = snapshot.worlds[i].name;
        }
        if (click.remove) {
            pendingDelete = snapshot.worlds[i].name;
        }
    }

    EndScissorMode();
    if (!pendingDelete.empty()) GuiUnlock();

    if (hasFreshNotice(snapshot)) {
        DrawText(snapshot.notice.c_str(), 30, screenH - 34, 16, snapshot.noticeIsError ? errorColor : mutedColor);
    }

    if (!pendingDelete.empty()) {
        DrawRectangle(0, 0, screenW, screenH, Color{0, 0, 0, 150});

        const int boxW = 420;
        const int boxH = 130;
        const int boxX = screenW / 2 - boxW / 2;
        const int boxY = screenH / 2 - boxH / 2;
        DrawRectangle(boxX, boxY, boxW, boxH, Color{18, 18, 20, 255});
        DrawRectangleLines(boxX, boxY, boxW, boxH, textColor);
        DrawText(TextFormat("Delete world '%s'? This cannot be undone.", pendingDelete.c_str()), boxX + 20,
                 boxY + 16, 16, textColor);

        const bool confirmDelete = GuiButton(
            Rectangle{static_cast<float>(boxX) + 20, static_cast<float>(boxY) + 56, 180, 30}, "Delete");
        const bool cancelDelete = GuiButton(
            Rectangle{static_cast<float>(boxX) + 220, static_cast<float>(boxY) + 56, 180, 30}, "Cancel (Esc)");

        if (confirmDelete) {
            network.sendDeleteWorld(pendingDelete);
            pendingDelete.clear();
        } else if (cancelDelete) {
            pendingDelete.clear();
        }
    }

    if (!selected.empty()) {
        network.sendStartSimulation(selected);
        return AppScreen::World;
    }

    return AppScreen::WorldSelect;
}

} // namespace WorldSelectScreen

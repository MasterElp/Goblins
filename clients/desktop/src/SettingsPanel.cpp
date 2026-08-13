#define RAYGUI_IMPLEMENTATION
#include "SettingsPanel.hpp"

#include <raygui.h>

namespace {

constexpr float kRowHeight = 36.0f;
constexpr float kSectionGap = 10.0f;
constexpr float kLabelWidth = 125.0f;

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

    // Immediate-mode: сначала считаем, сколько всего строк контента, чтобы
    // задать GuiScrollPanel настоящую высоту — иначе скролл не появится.
    // 2 (seed) + 1 (boulder count) + 5 (freq) + 3 (fractal) + 2 (bumps) +
    // 5 (river) + 4 (pond) + 3 (moisture) + 3 (minerals) = 28 строк с
    // параметрами, плюс 8 заголовков секций.
    constexpr int kParamRows = 28;
    constexpr int kSectionHeaders = 8;
    const float contentHeight = kParamRows * kRowHeight + kSectionHeaders * (kRowHeight + kSectionGap) + kSectionGap;

    static Rectangle view{};
    const Rectangle content{0, 0, scrollBounds.width - 18, contentHeight};

    GuiScrollPanel(scrollBounds, "Generation settings", content, &scroll_, &view);

    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y), static_cast<int>(view.width),
                      static_cast<int>(view.height));

    float y = scrollBounds.y + scroll_.y;
    const float x = scrollBounds.x + scroll_.x + 8;
    const float rowWidth = scrollBounds.width - 34;

    auto section = [&](const char* title) {
        GuiLabel(Rectangle{x, y, rowWidth, kRowHeight}, title);
        y += kRowHeight + kSectionGap;
    };

    auto floatRow = [&](const char* label, float& value, float lo, float hi) {
        GuiLabel(Rectangle{x, y, rowWidth, 16}, TextFormat("%s: %.4f", label, value));
        GuiSliderBar(Rectangle{x, y + 17, rowWidth, kRowHeight - 21}, nullptr, nullptr, &value, lo, hi);
        y += kRowHeight;
    };

    auto intRow = [&](const char* label, int& value, int lo, int hi) {
        float f = static_cast<float>(value);
        GuiLabel(Rectangle{x, y, rowWidth, 16}, TextFormat("%s: %d", label, value));
        GuiSliderBar(Rectangle{x, y + 17, rowWidth, kRowHeight - 21}, nullptr, nullptr, &f, static_cast<float>(lo),
                     static_cast<float>(hi));
        value = static_cast<int>(f + 0.5f);
        y += kRowHeight;
    };

    auto unsignedSeedRow = [&](const char* label, unsigned& value) {
        float f = static_cast<float>(value);
        GuiLabel(Rectangle{x, y, rowWidth - 70, 16}, TextFormat("%s: %u", label, value));
        bool randomPressed = GuiButton(Rectangle{x + rowWidth - 60, y - 2, 60, 20}, "Random");
        GuiSliderBar(Rectangle{x, y + 17, rowWidth, kRowHeight - 21}, nullptr, nullptr, &f, 0.0f, 999999.0f);
        if (randomPressed) {
            value = static_cast<unsigned>(GetRandomValue(0, 999999));
        } else {
            value = static_cast<unsigned>(f + 0.5f);
        }
        y += kRowHeight;
    };

    section("Seeds");
    unsignedSeedRow("Terrain seed", edited_.terrain_seed);
    unsignedSeedRow("Boulder seed", edited_.boulder_seed);

    section("Boulders");
    intRow("Boulder count", edited_.boulder_count, 0, 300);

    section("Noise frequency (smaller = bigger shapes)");
    floatRow("Height", edited_.terrain.height_noise_frequency, 0.002f, 0.2f);
    floatRow("Rockiness", edited_.terrain.rock_noise_frequency, 0.002f, 0.2f);
    floatRow("Compaction", edited_.terrain.compaction_noise_frequency, 0.002f, 0.2f);
    floatRow("Moisture", edited_.terrain.moisture_noise_frequency, 0.002f, 0.2f);
    floatRow("Minerals", edited_.terrain.minerals_noise_frequency, 0.002f, 0.2f);

    section("Fractal noise shape");
    intRow("Octaves", edited_.terrain.noise_octaves, 1, 8);
    floatRow("Lacunarity", edited_.terrain.noise_lacunarity, 1.0f, 4.0f);
    floatRow("Gain", edited_.terrain.noise_gain, 0.1f, 0.9f);

    section("Height bumps (keeps river off rock/packed ground)");
    floatRow("Rock bump", edited_.terrain.rock_height_bump, 0.0f, 1.0f);
    floatRow("Compaction bump", edited_.terrain.compaction_height_bump, 0.0f, 1.0f);

    section("River");
    intRow("Count", edited_.terrain.river_count, 0, 20);
    floatRow("Width (tiles)", edited_.terrain.river_width, 1.0f, 12.0f);
    floatRow("Sinuosity", edited_.terrain.river_sinuosity, 0.0f, 1.0f);
    floatRow("Depth", edited_.terrain.river_depth, 0.2f, 5.0f);
    floatRow("Max flow speed", edited_.terrain.river_max_flow_speed, 0.1f, 10.0f);

    section("Ponds");
    floatRow("Min depth", edited_.terrain.min_pond_depth, 0.0f, 0.5f);
    intRow("Min size (tiles)", edited_.terrain.min_pond_size, 1, 200);
    intRow("Max size (0=none)", edited_.terrain.max_pond_size, 0, 2000);
    floatRow("Depth scale", edited_.terrain.pond_depth_scale, 0.5f, 10.0f);

    section("Moisture");
    floatRow("Falloff (tiles)", edited_.terrain.moisture_falloff, 1.0f, 40.0f);
    floatRow("Water boost", edited_.terrain.water_moisture_boost, 0.0f, 1.0f);
    floatRow("Rock reduction", edited_.terrain.rock_moisture_reduction, 0.0f, 1.0f);

    section("Minerals");
    floatRow("Average", edited_.terrain.minerals_average, 0.0f, 50.0f);
    intRow("River value", edited_.terrain.river_minerals, 0, 50);
    // Свойство мира (06_GameLoop.md, п.1a), не текущий параметр
    // симуляции: выбирается здесь, при генерации, и во время самой
    // симуляции HydrologySystem его больше не меняет.
    floatRow("Moisture threshold", edited_.terrain.mineral_moisture_threshold, 0.0f, 1.0f);

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

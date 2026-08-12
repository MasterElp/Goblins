#define RAYGUI_IMPLEMENTATION
#include "SettingsPanel.hpp"

#include <raygui.h>

namespace {

constexpr float kRowHeight = 22.0f;
constexpr float kSectionGap = 10.0f;
constexpr float kLabelWidth = 150.0f;

} // namespace

void SettingsPanel::loadFrom(const goblins::RegenerationRequest& current, bool force) {
    if (loaded_ && !force) {
        return;
    }
    edited_ = current;
    loaded_ = true;
}

bool SettingsPanel::draw(Rectangle bounds, goblins::RegenerationRequest& outRequest) {
    if (!visible) {
        return false;
    }

    // Immediate-mode: сначала считаем, сколько всего строк контента, чтобы
    // задать GuiScrollPanel настоящую высоту — иначе скролл не появится.
    // 2 (seed) + 1 (boulder count) + 4 (freq) + 3 (fractal) + 2 (bumps) +
    // 4 (river) + 4 (pond) + 3 (moisture) = 23 строки с параметрами,
    // плюс 7 заголовков секций и 2 кнопки внизу.
    constexpr int kParamRows = 23;
    constexpr int kSectionHeaders = 7;
    const float contentHeight =
        kParamRows * kRowHeight + kSectionHeaders * (kRowHeight + kSectionGap) + 3 * (kRowHeight + kSectionGap);

    static Rectangle view{};
    const Rectangle content{0, 0, bounds.width - 18, contentHeight};

    GuiScrollPanel(bounds, "Generation settings", content, &scroll_, &view);

    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y), static_cast<int>(view.width),
                      static_cast<int>(view.height));

    float y = bounds.y + scroll_.y;
    const float x = bounds.x + scroll_.x + 8;
    const float rowWidth = bounds.width - 34;

    auto section = [&](const char* title) {
        GuiLabel(Rectangle{x, y, rowWidth, kRowHeight}, title);
        y += kRowHeight + kSectionGap;
    };

    auto floatRow = [&](const char* label, float& value, float lo, float hi) {
        GuiSlider(Rectangle{x + kLabelWidth, y, rowWidth - kLabelWidth, kRowHeight - 4}, label,
                  TextFormat("%.4f", value), &value, lo, hi);
        y += kRowHeight;
    };

    auto intRow = [&](const char* label, int& value, int lo, int hi) {
        float f = static_cast<float>(value);
        GuiSlider(Rectangle{x + kLabelWidth, y, rowWidth - kLabelWidth, kRowHeight - 4}, label,
                  TextFormat("%d", value), &f, static_cast<float>(lo), static_cast<float>(hi));
        value = static_cast<int>(f + 0.5f);
        y += kRowHeight;
    };

    auto unsignedSeedRow = [&](const char* label, unsigned& value) {
        float f = static_cast<float>(value);
        GuiSlider(Rectangle{x + kLabelWidth, y, rowWidth - kLabelWidth - 70, kRowHeight - 4}, label,
                  TextFormat("%u", value), &f, 0.0f, 999999.0f);
        value = static_cast<unsigned>(f + 0.5f);
        if (GuiButton(Rectangle{x + rowWidth - 60, y, 60, kRowHeight - 4}, "Random")) {
            value = static_cast<unsigned>(GetRandomValue(0, 999999));
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

    section("Fractal noise shape");
    intRow("Octaves", edited_.terrain.noise_octaves, 1, 8);
    floatRow("Lacunarity", edited_.terrain.noise_lacunarity, 1.0f, 4.0f);
    floatRow("Gain", edited_.terrain.noise_gain, 0.1f, 0.9f);

    section("Height bumps (keeps river off rock/packed ground)");
    floatRow("Rock bump", edited_.terrain.rock_height_bump, 0.0f, 1.0f);
    floatRow("Compaction bump", edited_.terrain.compaction_height_bump, 0.0f, 1.0f);

    section("River");
    floatRow("Threshold", edited_.terrain.river_threshold, 5.0f, 300.0f);
    floatRow("Depth base", edited_.terrain.river_depth_base, 0.0f, 2.0f);
    floatRow("Depth range", edited_.terrain.river_depth_range, 0.0f, 5.0f);
    floatRow("Edge inflow max", edited_.terrain.edge_inflow_max, 0.0f, 200.0f);

    section("Ponds");
    floatRow("Min depth", edited_.terrain.min_pond_depth, 0.0f, 0.5f);
    intRow("Min size (tiles)", edited_.terrain.min_pond_size, 1, 200);
    intRow("Max size (0=none)", edited_.terrain.max_pond_size, 0, 2000);
    floatRow("Depth scale", edited_.terrain.pond_depth_scale, 0.5f, 10.0f);

    section("Moisture");
    floatRow("Falloff (tiles)", edited_.terrain.moisture_falloff, 1.0f, 40.0f);
    floatRow("Water boost", edited_.terrain.water_moisture_boost, 0.0f, 1.0f);
    floatRow("Rock reduction", edited_.terrain.rock_moisture_reduction, 0.0f, 1.0f);

    y += kSectionGap;
    bool regenerate = false;
    if (GuiButton(Rectangle{x, y, rowWidth, 30}, "Regenerate")) {
        regenerate = true;
    }
    y += 30 + 6;
    if (GuiButton(Rectangle{x, y, rowWidth, 26}, "Reset to current server state")) {
        loaded_ = false; // следующий loadFrom(..., force=false) перезапишет edited_
    }

    EndScissorMode();

    if (regenerate) {
        outRequest = edited_;
        return true;
    }
    return false;
}

#include "SelectionCard.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

#include "TileColors.hpp"

namespace SelectionCard {

namespace {

constexpr float kWidth = 300.0f;
constexpr float kPad = 10.0f;
constexpr int kTitleFont = 16;
constexpr int kFont = 13;
constexpr float kRow = 17.0f;
constexpr float kMargin = 10.0f;

// Те же краски, что у панели справа: одно и то же существо не должно менять
// цвет от того, в каком окне на него смотрят.
const Color kBack{18, 18, 20, 235};
const Color kEdge{90, 90, 98, 255};
const Color kTitleColor{235, 200, 110, 255};
const Color kMutedColor{135, 135, 142, 255};
const Color kValueColor{245, 245, 245, 255};
const Color kDoingColor{170, 220, 160, 255};
const Color kGoneColor{230, 130, 120, 255};

// Сколько строк под заголовком у этой цели.
//
// Высота считается по виду цели, а не по тому, сколько строк вышло: карточка
// стои́т на карте, и меняйся её размер от того, ответил ли уже сервер про
// занятие, — она бы дёргалась сама по себе. От вида к виду размер меняется
// только вместе с выбором, то есть по воле руки.
int rowsFor(InfoPanel::Target::Kind kind) {
    switch (kind) {
        case InfoPanel::Target::Kind::Animal:
            return 3; // пол с желанием, целость, взрослость
        case InfoPanel::Target::Kind::Goblin:
            return 6; // то же плюс усталость и ноша
        case InfoPanel::Target::Kind::Plant:
            return 2; // развитость и влажность клетки
        case InfoPanel::Target::Kind::Soil:
            return 3; // влажность, каменистость, минералы
        case InfoPanel::Target::Kind::None:
            return 0;
    }
    return 0;
}

// Полоска доли: имя слева, полоса посередине, проценты справа.
//
// Полосой, а не числом: карточку читают краем глаза, не отрываясь от карты, и
// «половина» видна раньше, чем прочитано «52%». Число рядом всё же есть —
// по полосе не отличить 52% от 55%, а разглядывать существо приходят и за
// этим тоже.
void bar(float x, float y, float width, const char* name, float value, Color fill) {
    const float share = std::clamp(value, 0.0f, 1.0f);
    constexpr float kNameWidth = 58.0f;
    constexpr float kPercentWidth = 38.0f;
    const float barX = x + kNameWidth;
    const float barW = std::max(10.0f, width - kNameWidth - kPercentWidth);
    DrawText(name, static_cast<int>(x), static_cast<int>(y), kFont, kMutedColor);
    DrawRectangle(static_cast<int>(barX), static_cast<int>(y) + 3, static_cast<int>(barW), 9,
                  Color{45, 45, 50, 255});
    DrawRectangle(static_cast<int>(barX), static_cast<int>(y) + 3,
                  std::max(1, static_cast<int>(barW * share)), 9, fill);
    const char* percent = TextFormat("%.0f%%", share * 100.0f);
    DrawText(percent, static_cast<int>(x + width) - MeasureText(percent, kFont), static_cast<int>(y), kFont,
             kValueColor);
}

// Цвет целости: от зелёного к красному. Раненое существо должно бросаться в
// глаза — за ним чаще всего и следят.
Color healthColor(float health) {
    const float share = std::clamp(health, 0.0f, 1.0f);
    return Color{static_cast<unsigned char>(220 - 110 * share), static_cast<unsigned char>(90 + 110 * share),
                 static_cast<unsigned char>(80 + 40 * share), 255};
}

// Чем гоблин занят прямо сейчас — готовой строкой от сервера
// (WorldState::watched). Только гоблин: у зверя такой строки в протоколе нет
// вовсе (см. "doing" в server/NetworkServer.hpp), и строка ожидания под
// каждым зверем висела бы вечно, обещая то, что не придёт.
//
// Пока она не приехала, честнее сказать об этом, чем оставить пустое место:
// пустая строка в карточке читается как «ничего не делает».
void drawDoing(const WorldState& state, const InfoPanel::Target& target, float x, float y, float width) {
    if (!state.watched.doing.empty() && InfoPanel::watchedMatches(state, target)) {
        DrawText(state.watched.doing.c_str(), static_cast<int>(x), static_cast<int>(y), kFont, kDoingColor);
        return;
    }
    const char* waiting = state.watched.kind == "gone" ? "no longer in the world" : "asking the server...";
    DrawText(waiting, static_cast<int>(x), static_cast<int>(y), kFont,
             state.watched.kind == "gone" ? kGoneColor : kMutedColor);
    (void)width;
}

} // namespace

Rectangle bounds(const InfoPanel::Target& target, Rectangle viewport) {
    const int rows = rowsFor(target.kind);
    if (rows == 0) {
        return Rectangle{0, 0, 0, 0};
    }
    const float height = kPad + static_cast<float>(kTitleFont) + 6.0f + static_cast<float>(rows) * kRow + kPad;
    const float width = std::min(kWidth, viewport.width - 2.0f * kMargin);
    // Не влезает — не рисуем вовсе. Окно бывает и в половину открытки, и
    // карточка, занявшая его целиком, закрыла бы ровно то, ради чего в него
    // смотрят.
    if (width < 160.0f || height > viewport.height - 2.0f * kMargin) {
        return Rectangle{0, 0, 0, 0};
    }
    return Rectangle{viewport.x + kMargin, viewport.y + viewport.height - height - kMargin, width, height};
}

void draw(const WorldState& state, const InfoPanel::Target& target, Rectangle bounds) {
    if (bounds.width <= 0.0f) {
        return;
    }
    DrawRectangleRec(bounds, kBack);
    DrawRectangleLinesEx(bounds, 1.0f, kEdge);

    const InfoPanel::Heading heading = InfoPanel::headingOf(state, target);
    const float x = bounds.x + kPad;
    const float width = bounds.width - 2.0f * kPad;
    float y = bounds.y + kPad;

    float titleX = x;
    if (heading.swatch.a > 0) {
        DrawRectangle(static_cast<int>(x), static_cast<int>(y) + 3, 11, 11, heading.swatch);
        titleX += 16.0f;
    }
    DrawText(heading.title.c_str(), static_cast<int>(titleX), static_cast<int>(y), kTitleFont,
             heading.gone ? kGoneColor : kTitleColor);
    {
        const char* at = TextFormat("(%d,%d)", heading.x, heading.y);
        DrawText(at, static_cast<int>(x + width) - MeasureText(at, kFont), static_cast<int>(y) + 3, kFont,
                 kMutedColor);
    }
    y += static_cast<float>(kTitleFont) + 6.0f;

    // Пол с желанием — одной строкой у зверя и у гоблина: тело у них одно
    // (core/Body.hpp), и разной строки оно не заслуживает.
    const auto bodyLines = [&](const std::string& sex, const std::string& desire, float health, float growth,
                               bool withDoing) {
        DrawText(TextFormat("%s   wants %s", sex.c_str(), desire.c_str()), static_cast<int>(x),
                 static_cast<int>(y), kFont, kMutedColor);
        y += kRow;
        if (withDoing) {
            drawDoing(state, target, x, y, width);
            y += kRow;
        }
        bar(x, y, width, "health", health, healthColor(health));
        y += kRow;
        bar(x, y, width, "grown", growth, Color{150, 210, 255, 255});
        y += kRow;
    };

    switch (target.kind) {
        case InfoPanel::Target::Kind::Animal: {
            const WorldState::Animal* animal = InfoPanel::findAnimal(state, target.animalId);
            if (animal != nullptr) {
                bodyLines(animal->sex, animal->desire, animal->health, animal->growth, false);
            }
            break;
        }
        case InfoPanel::Target::Kind::Goblin: {
            const WorldState::Goblin* goblin = InfoPanel::findGoblin(state, target.animalId);
            if (goblin != nullptr) {
                bodyLines(goblin->sex, goblin->desire, goblin->health, goblin->growth, true);
                bar(x, y, width, "tired", goblin->fatigue, Color{235, 180, 90, 255});
                y += kRow;
                // Ноша — то, чего нет у зверя вовсе, и то, ради чего за
                // гоблином чаще всего и следят: несёт ли он что-нибудь домой.
                const bool empty = goblin->carried <= 0.0f && goblin->material <= 0.0f;
                DrawText(empty ? "carries nothing"
                               : TextFormat("carries %.1f food   %.1f material", goblin->carried,
                                            goblin->material),
                         static_cast<int>(x), static_cast<int>(y), kFont, empty ? kMutedColor : kValueColor);
                y += kRow;
            }
            break;
        }
        case InfoPanel::Target::Kind::Plant: {
            const std::size_t cell = static_cast<std::size_t>(heading.y) * state.areaWidth + heading.x;
            if (!heading.gone && cell < state.plantGrowth.size() && cell < state.moisture.size()) {
                bar(x, y, width, "grown", state.plantGrowth[cell], Color{150, 210, 255, 255});
                y += kRow;
                // Влажность клетки, а не свойство самого растения, — и это
                // главное, что о нём стоит знать: ею решается, вырастет оно
                // тут или засохнет.
                bar(x, y, width, "moisture", state.moisture[cell], Color{110, 165, 195, 255});
                y += kRow;
            }
            break;
        }
        case InfoPanel::Target::Kind::Soil: {
            const std::size_t cell = static_cast<std::size_t>(heading.y) * state.areaWidth + heading.x;
            if (heading.x >= 0 && heading.y >= 0 && heading.x < state.areaWidth &&
                heading.y < state.areaHeight && cell < state.moisture.size() &&
                cell < state.rockiness.size()) {
                bar(x, y, width, "moisture", state.moisture[cell], Color{110, 165, 195, 255});
                y += kRow;
                bar(x, y, width, "rocky", state.rockiness[cell], Color{160, 160, 168, 255});
                y += kRow;
                const int minerals = cell < state.minerals.size() ? state.minerals[cell] : 0;
                const int humus = cell < state.humus.size() ? state.humus[cell] : 0;
                DrawText(TextFormat("minerals %d   humus %d", minerals, humus), static_cast<int>(x),
                         static_cast<int>(y), kFont, kValueColor);
                y += kRow;
            }
            break;
        }
        case InfoPanel::Target::Kind::None:
            break;
    }
}

} // namespace SelectionCard

#include "core/systems/HydrologySystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

#include "core/Moisture.hpp"
#include "core/components/HeightComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WaterSourceComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Поправки на расстояние до диагонального соседа (прежний kDist8) здесь
// больше нет и быть не должно: уклон в этой системе нигде не считается.
// Вода не выбирает направление и не течёт с какой-то скоростью — она
// встаёт на один уровень внутри "лужи" (см. шаг 5), а для уровня важно
// только то, кто с кем граничит, а не насколько сосед далеко.

// Влажность: цель — общая с генерацией функция moistureTarget
// (core/Moisture.hpp), одна на весь проект. Здесь она не применяется
// разом, а служит целью, к которой влажность движется по
// kMoistureAdaptRate за тик: вода передвинулась — почва подсыхает или
// увлажняется постепенно, а не мгновенно.
constexpr float kMoistureAdaptRate = 0.01f;

// Утрамбованности в почве больше нет, и размягчать её у воды больше
// некому. Она жила ради одного потребителя — пригодности почвы для
// растения, — а та оказалась вторым способом сказать то, что уже говорит
// влажность (см. SoilComponent). Вместе с ней ушли три константы:
// kCompactionRockFloor, kCompactionSoftenReach, kCompactionSoftenRate.

// Ниже этой глубины воды на тайле нет (WaterComponent отсутствует —
// 02_CorePrinciples.md, п.3). Порог ОДИН, а не пара "появиться/исчезнуть":
// с двумя порогами вода, натёкшая на сухой тайл в объёме между ними, не
// записывалась никуда вовсе — компонент не создавался, глубина терялась, а
// отправитель её у себя уже списал. Кромка воды каждый тик съедала часть
// массы, и растекаться постепенно вода не могла: ей нужно было прийти
// куском больше верхнего порога за один тик, отсюда резкая граница
// "глубоко / сухо" вместо мелководья. Теряется теперь только то, что ниже
// самого порога, — меньше, чем испаряется за 25 тиков.
constexpr float kWaterMinDepth = 0.001f;

// Сколько клеток в "луже" — сама клетка и восемь соседей. Делить сумму
// правок надо именно на это ПОСТОЯННОЕ число, а не на то, во скольких
// группах клетка реально участвовала: внутри одной группы сумма правок
// равна нулю по построению (вода только перераспределяется), поэтому
// деление на общую константу оставляет нулевой и сумму по всей карте —
// вода не появляется и не исчезает на кромке водоёма, где число групп у
// клеток разное.
constexpr int kPoolCells = 9;

// Сколько воды почва удерживает у себя и НЕ отдаёт течению — плёнка,
// смачивающая грунт. Течёт только то, что выше этой глубины.
//
// Без удержания тончайшая плёнка на суше (доли сотой глубины) бесконечно
// переливалась туда-обратно между соседями: разница уровней у неё
// микроскопическая, но ненулевая, поэтому каждый тик кто-то кому-то
// что-то отдавал, а на следующий тик — обратно. На карте это выглядело
// как мелкая вода, суетливо бегающая по суше. С удержанием такая плёнка
// просто лежит и испаряется, как настоящая лужа после дождя, а заодно
// перестаёт бесконечно расползаться дальше по сухой земле.
//
// Величина заведомо мала по сравнению с руслом (глубина реки — единицы),
// поэтому на само течение рек и прудов не влияет.
constexpr float kWaterRetention = 0.02f;

// Край карты — обрыв в пустоту (kVoidHeight из HeightComponent.hpp,
// docs/01_Cosmology.md): Область висит в космосе островом, и за её границей
// нет ни земли, ни дна. Тайлы самой границы держатся на нулевой высоте и
// нулевой глубине: всё, что туда натекло, за тот же тик падает вниз и в мир
// не возвращается.
//
// Раньше на этом месте была доля стока (kEdgeDrainRate): край вёл себя как
// пологий слив, вода у границы всё равно копилась, и приходилось подбирать
// скорость. Обрыв ничего подбирать не требует — он просто край мира.

// Порог влажности соседнего тайла, при котором минералы начинают
// "вымываться" в его сторону — SoilComponent.moisture нормализована в
// [0, 1], поэтому 0.5 здесь и есть те "50" из исходного запроса на
// минералы.
constexpr float kMineralMoistureThreshold = 0.5f;

// Минералы: выравнивание с единственным соседом за тик, а не раздача
// всем сразу. Клетка A ищет среди соседей, которые "притягивают" минералы
// (есть вода или влажность выше порога — они "вымываются" туда), самого
// бедного B; если A богаче B минимум на kMineralSlopeThreshold, к B уходит
// ровно половина разницы (целочисленно) — 8 и 6 становятся 7 и 7, 10 и 6
// — 8 и 8. Обязательно ОДИН сосед, не несколько: если считать перетоки к
// нескольким соседям независимо по одному и тому же снимку (как было
// раньше), клетка может отдать больше, чем у неё есть, и вместо
// предсказуемого сглаживания получаются на вид хаотичные "волны" — тот же
// приём единственного лучшего соседа, что и у течения воды выше.
constexpr int kMineralSlopeThreshold = 2;

// Дождь. Настраиваются только "как часто" и "сколько" (свойства мира,
// WorldPropertiesComponent); а вот ФОРМА дождя — закон, а не настройка:
// он идёт kRainDurationTicks тиков подряд и каждый такой тик роняет
// kRainDropsPerTick капель в случайные клетки.
//
// Именно форма важнее величины. Дождь, разом добавлявший глубину КАЖДОМУ
// тайлу карты, мгновенно превращал в воду весь мир: трава тонула целиком,
// луга выкашивало под ноль каждый раз. Редкие капли, растянутые по
// времени, дают лужи, которые стекают в низины и русла и высыхают, — при
// этом бо́льшая часть карты за один дождь не намокает вовсе.
constexpr int kRainDurationTicks = 60;
constexpr int kRainDropsPerTick = 20;
// Отличает поток случайности дождя от посевного (PlantSystem) — тот же
// приём mixSeed/randomUnit, что и там.
constexpr std::uint64_t kRainSalt = 0x5261696E44726F70ull;

} // namespace

void HydrologySystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    // Свойства мира, выбранные один раз при генерации — System только
    // читает, никогда не пишет (06_GameLoop.md, п.1a).
    const auto& worldProperties = world.registry().get<const WorldPropertiesComponent>(world.worldEntity());
    const float waterSourceDepth = worldProperties.waterSourceDepth;
    const float waterEvaporationRate = worldProperties.waterEvaporationRate;
    const int rainIntervalTicks = worldProperties.rainIntervalTicks;
    const float rainAmount = worldProperties.rainAmount;
    const float soilErosionRate = worldProperties.soilErosionRate;
    const float maxErosionDepth = worldProperties.maxErosionDepth;

    // --- 1. Снимок текущего состояния ---
    // entt::null не подставляется вторым аргументом vector(count, value)
    // напрямую: у него шаблонный неограниченный operator T(), из-за чего
    // MSVC при разборе перегрузок конструктора vector пытается
    // сконвертировать его ещё и в Allocator и падает с ошибкой внутри
    // entt.hpp — поэтому конвертируем в entt::entity явно, заранее.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> entities(cellCount, kNullEntity);
    std::vector<float> moisture(cellCount, 0.0f);
    std::vector<float> rockiness(cellCount, 0.0f);
    std::vector<int> minerals(cellCount, 0);
    std::vector<float> terrainHeight(cellCount, 0.0f);
    std::vector<float> waterDepth(cellCount, 0.0f);
    std::vector<bool> isWaterSource(cellCount, false);

    auto& registry = world.registry();
    auto view = registry.view<PositionComponent, SoilComponent, HeightComponent>();
    for (const auto entity : view) {
        const auto& position = view.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        const auto& soil = view.get<SoilComponent>(entity);
        const auto& heightComponent = view.get<HeightComponent>(entity);

        entities[i] = entity;
        moisture[i] = soil.moisture;
        rockiness[i] = soil.rockiness;
        minerals[i] = soil.minerals;
        terrainHeight[i] = heightComponent.height;

        if (const auto* water = registry.try_get<WaterComponent>(entity)) {
            waterDepth[i] = water->depth;
        }
        isWaterSource[i] = registry.all_of<WaterSourceComponent>(entity);
    }

    // --- 2. Дистанция до воды: multi-source BFS (8-связность), заново
    // каждый тик — вода двигается, поэтому карта дистанций не статична. ---
    std::vector<int> distanceToWater(cellCount, -1);
    std::queue<int> bfs;
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (waterDepth[i] > 0.0f) {
            distanceToWater[i] = 0;
            bfs.push(static_cast<int>(i));
        }
    }
    while (!bfs.empty()) {
        const int idx = bfs.front();
        bfs.pop();
        const int x = idx % width;
        const int y = idx / width;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t ni = index(nx, ny);
            if (distanceToWater[ni] == -1) {
                distanceToWater[ni] = distanceToWater[static_cast<std::size_t>(idx)] + 1;
                bfs.push(static_cast<int>(ni));
            }
        }
    }

    // --- 3. Влажность: релаксация к цели ---
    std::vector<float> nextMoisture(moisture);
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (entities[i] == entt::null) {
            continue;
        }

        const float target = moistureTarget(distanceToWater[i], rockiness[i]);
        nextMoisture[i] = moisture[i] + (target - moisture[i]) * kMoistureAdaptRate;
    }

    // --- 5. Течение: вода в "луже" встаёт на один уровень.
    //
    // Закон один и предельно простой: берём клетку с водой и тех её восьмерых
    // соседей, куда вода вообще может попасть, и разливаем всю воду этой
    // группы так, чтобы ПОВЕРХНОСТЬ (дно + вода) везде в ней стала одинаковой.
    // Где дно ниже — глубже, где дно выше уровня — сухо. Это и есть "вода
    // находит свой уровень"; ни направления стока, ни скорости, ни выбора
    // одного соседа из восьми здесь больше нет.
    //
    // Сосед участвует, если у него УЖЕ есть вода (лужи сливаются) или если
    // его ДНО ниже нашей текущей поверхности (воде есть куда пролиться).
    // Второе условие и есть единственная дверь на сушу: подняться выше
    // собственного уровня вода не может, поэтому берег выше кромки (при
    // генерации русло вдавлено на kRiverFreeboard ниже земли) держит
    // реку в русле сам, без запретов.
    //
    // Группы соседних клеток перекрываются, поэтому правка каждой клетки —
    // сумма её правок по всем группам, делённая на постоянное kPoolCells
    // (см. комментарий у константы: только постоянный делитель сохраняет
    // объём воды). Внутри группы выравнивание полное, за один тик; общая
    // же картина доходит до ровного уровня за несколько тиков — и именно
    // это усреднение по перекрывающимся группам гасит рябь, из-за которой
    // прежний вариант с выбором одного соседа приходилось чинить в три
    // прохода. ---
    std::vector<float> nextWaterDepth(waterDepth);
    std::vector<float> nextTerrainHeight(terrainHeight);

    // Сумма правок глубины по всем группам, куда попала клетка (делится на
    // kPoolCells ниже), и самое низкое ЧУЖОЕ дно среди них — потолок для
    // размыва и осаждения в 5.1.
    std::vector<float> depthDelta(cellCount, 0.0f);
    std::vector<float> lowestNeighborBed(cellCount, std::numeric_limits<float>::infinity());

    // Буферы одной группы: сама клетка плюс до восьми соседей.
    int poolCell[kPoolCells];
    float poolBed[kPoolCells];
    float poolMobile[kPoolCells];
    int poolOrder[kPoolCells];

    for (std::size_t i = 0; i < cellCount; ++i) {
        // Течёт только вода выше удерживаемой почвой плёнки
        // (kWaterRetention): её "дном" для разлива служит верх этой плёнки,
        // сама она остаётся на месте.
        if (waterDepth[i] - kWaterRetention <= 0.0f) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;
        const float surface = terrainHeight[i] + waterDepth[i];

        auto addMember = [&](std::size_t k, int& count) {
            const float held = std::min(waterDepth[k], kWaterRetention);
            poolCell[count] = static_cast<int>(k);
            poolBed[count] = terrainHeight[k] + held;
            poolMobile[count] = waterDepth[k] - held;
            ++count;
        };

        int members = 0;
        addMember(i, members);
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t ni = index(nx, ny);
            if (waterDepth[ni] <= 0.0f && terrainHeight[ni] >= surface) {
                continue; // сухо и выше нашего уровня — воде туда не подняться
            }
            addMember(ni, members);
            lowestNeighborBed[i] = std::min(lowestNeighborBed[i], terrainHeight[ni]);
            lowestNeighborBed[ni] = std::min(lowestNeighborBed[ni], terrainHeight[i]);
        }
        if (members < 2) {
            continue;
        }

        // Уровень L, при котором вся подвижная вода группы разлита ровно:
        // sum(max(0, L - дно_k)) = sum(подвижная_k). Идём по дну снизу
        // вверх — как только пробный уровень не достаёт до следующего по
        // высоте дна, он и есть ответ (эта клетка и все выше неё остаются
        // сухими).
        float volume = 0.0f;
        for (int n = 0; n < members; ++n) {
            volume += poolMobile[n];
            poolOrder[n] = n;
        }
        for (int a = 1; a < members; ++a) { // сортировка вставками: members <= 9
            const int key = poolOrder[a];
            int b = a - 1;
            while (b >= 0 && poolBed[poolOrder[b]] > poolBed[key]) {
                poolOrder[b + 1] = poolOrder[b];
                --b;
            }
            poolOrder[b + 1] = key;
        }

        float level = poolBed[poolOrder[0]];
        float bedSum = 0.0f;
        for (int k = 0; k < members; ++k) {
            bedSum += poolBed[poolOrder[k]];
            const float candidate = (volume + bedSum) / static_cast<float>(k + 1);
            if (k + 1 == members || candidate <= poolBed[poolOrder[k + 1]]) {
                level = candidate;
                break;
            }
        }

        for (int n = 0; n < members; ++n) {
            const float target = std::max(0.0f, level - poolBed[n]);
            depthDelta[static_cast<std::size_t>(poolCell[n])] += target - poolMobile[n];
        }
    }

    // --- 5.1 Применение: вода, эрозия, осаждение ---
    // Итоговая правка глубины — сумма по всем группам, делённая на
    // постоянное kPoolCells. Отсюда же берётся и "сколько воды через клетку
    // прошло": отдала (правка < 0) или приняла (правка > 0).
    //
    // Эрозия и осаждение — две ветки одного закона, а не разные механизмы:
    //   клетка ОТДАЁТ воду, и её дно не ниже соседского -> порода мешает
    //     течению, вымываем;
    //   клетка ПРИНИМАЕТ воду, а рядом дно выше -> здесь углубление, туда
    //     оседает нанос.
    // Сколько за тик суммарно вымыло, столько же и раздаётся по
    // углублениям.
    float totalEroded = 0.0f;
    std::vector<float> depositCapacity(cellCount, 0.0f);
    float totalCapacity = 0.0f;

    for (std::size_t i = 0; i < cellCount; ++i) {
        const float delta = depthDelta[i] / static_cast<float>(kPoolCells);
        if (delta == 0.0f) {
            continue;
        }
        nextWaterDepth[i] += delta;

        if (delta >= 0.0f || std::isinf(lowestNeighborBed[i])) {
            continue; // приняла воду (или не с кем было делиться) — не размываем
        }
        // Размываем только там, где вода идёт НЕ по готовому руслу: если
        // дно клетки уже ниже соседского, вода и так течёт туда по уклону
        // самого рельефа, и углублять нечего.
        if (terrainHeight[i] > lowestNeighborBed[i]) {
            continue;
        }

        // Разная почва вымывается по-разному: камень сопротивляется
        // размыву, рыхлая земля уходит легко. Множителя утрамбованности
        // здесь больше нет — её самой больше нет в почве, — и склон горы
        // держится одной каменистостью, как и сказано в
        // docs/01_Cosmology.md, п.2.
        const float softness = 1.0f - rockiness[i];

        // Потолок выемки. Без него клетка под постоянным источником
        // размывается каждый тик без остановки — высота уезжает в минус, и
        // появляется бездонная яма, никак не связанная с рельефом вокруг.
        // Ограничиваем глубину относительно самого низкого соседа, с
        // которым клетка делится водой. Считаем от снимка (terrainHeight),
        // а не от накопителя, чтобы результат не зависел от порядка обхода
        // клеток — как и всё остальное в этом шаге.
        const float erosionFloor = lowestNeighborBed[i] - maxErosionDepth;
        const float allowedErosion = std::max(0.0f, terrainHeight[i] - erosionFloor);
        const float erosion = std::min(-delta * soilErosionRate * softness, allowedErosion);

        nextTerrainHeight[i] -= erosion;
        totalEroded += erosion;
    }

    // Ёмкость углублений: клетку, принявшую воду, разрешено поднять
    // максимум до дна самого низкого из её соседей — ровно "засыпать
    // ступеньку вровень", и ни крупицей выше. Нанос не может создать новый
    // бугор, а значит и новую рябь.
    for (std::size_t j = 0; j < cellCount; ++j) {
        if (entities[j] == entt::null || depthDelta[j] <= 0.0f || std::isinf(lowestNeighborBed[j])) {
            continue;
        }
        const float capacity = std::max(0.0f, lowestNeighborBed[j] - terrainHeight[j]);
        depositCapacity[j] = capacity;
        totalCapacity += capacity;
    }

    if (totalEroded > 0.0f && totalCapacity > 0.0f) {
        // Наноса больше, чем углублений, — излишку просто некуда осесть, и
        // он уходит с карты, как вода за край: Область — открытый кусок
        // мира, а не замкнутый сосуд, и требовать от неё точного баланса
        // почвы не за чем (02_CorePrinciples.md, п.12b).
        const float scale = std::min(1.0f, totalEroded / totalCapacity);
        for (std::size_t j = 0; j < cellCount; ++j) {
            if (depositCapacity[j] > 0.0f) {
                nextTerrainHeight[j] += depositCapacity[j] * scale;
            }
        }
    }

    // --- 5b. Приход и расход воды: испарение, источники, дождь. Всё —
    // независимые правки поверх nextWaterDepth из течения выше, и все
    // читают снимок (waterDepth/isWaterSource), а не друг друга и не
    // результат течения, поэтому порядок клеток не важен.
    //
    // Вместе они образуют баланс воды: сколько приносят источники и дожди,
    // столько же в равновесии уносят испарение и провал за край (5c ниже). ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (waterDepth[i] > 0.0f) {
            nextWaterDepth[i] -= waterEvaporationRate;
        }
    }

    // Дождь: раз в rainIntervalTicks тиков начинается дождь длиной
    // kRainDurationTicks, и каждый его тик роняет kRainDropsPerTick капель
    // по rainAmount глубины в случайные клетки. Идёт ли сейчас дождь и
    // куда падают капли, разыгрывается от номера тика и seed мира —
    // собственного "погодного" состояния, которое пришлось бы сохранять в
    // файл мира и поддерживать в согласии с остальным, у мира нет. Не
    // ради повторяемости прогонов (мир недетерминирован,
    // 02_CorePrinciples.md, п.12a), а ради того, чтобы у погоды не
    // заводилось скрытого состояния помимо самого мира.
    //
    // Дождь — приход воды ИЗВНЕ Области (02_CorePrinciples.md, п.12b): он
    // не берётся из здешней воды и ничего в мире не опустошает.
    //
    // Упавшая капля дальше живёт по общему закону: то, что выше
    // kWaterRetention, стекает по уклону в низины и русла, остальное лежит
    // лужей и испаряется.
    if (rainIntervalTicks > 0 && rainAmount > 0.0f) {
        const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
        const auto period = static_cast<std::uint64_t>(rainIntervalTicks);
        // Если дождь просят чаще, чем он длится, — идёт без перерыва.
        const auto duration = std::min<std::uint64_t>(static_cast<std::uint64_t>(kRainDurationTicks), period);
        if (tick % period < duration) {
            const auto worldSeed = static_cast<std::uint64_t>(worldProperties.plantRandomSeed);
            std::uint64_t rainState = mixSeed(worldSeed, mixSeed(tick, kRainSalt));
            for (int drop = 0; drop < kRainDropsPerTick; ++drop) {
                const float roll = randomUnit(rainState);
                std::size_t idx = static_cast<std::size_t>(roll * static_cast<float>(cellCount));
                if (idx >= cellCount) {
                    idx = cellCount - 1;
                }
                if (entities[idx] != entt::null) {
                    nextWaterDepth[idx] += rainAmount;
                }
            }
        }
    }

    // --- 5c. Источники и край мира — две границы Области, и обе жёстко
    // задают состояние своих клеток, а не правят его понемногу. Поэтому
    // они идут последними: что бы ни насчитали течение, испарение и дождь
    // выше, здесь оно переписывается набело.
    //
    // Источник (ледяная шапка на вершине или родник) — столб воды
    // постоянной глубины: сколько бы из него ни вытекло, к следующему тику
    // он снова полон. Край карты — обрыв в пустоту (kVoidHeight,
    // docs/01_Cosmology.md): нулевая высота, нулевая глубина, всё натёкшее
    // падает вниз и в мир не возвращается. Край сильнее источника: родник,
    // выпавший на самую границу, всё равно осыпается в космос. ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (isWaterSource[i]) {
            nextWaterDepth[i] = waterSourceDepth;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;
        if (x == 0 || x == width - 1 || y == 0 || y == height - 1) {
            nextTerrainHeight[i] = kVoidHeight;
            nextWaterDepth[i] = 0.0f;
        }
    }

    // --- 6. Минералы: снимок -> для каждой клетки найти ОДНОГО (самого
    // бедного из притягивающих) соседа -> аккумулятор — тот же приём, что
    // и течение воды выше, поэтому порядок обхода клеток не влияет на
    // результат. ---
    std::vector<int> nextMinerals(minerals);
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (entities[i] == entt::null || minerals[i] < kMineralSlopeThreshold) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;

        int bestNeighbor = -1;
        int bestMinerals = minerals[i];
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t j = index(nx, ny);
            const bool neighborAttractsMinerals = waterDepth[j] > 0.0f || moisture[j] > kMineralMoistureThreshold;
            if (!neighborAttractsMinerals || minerals[j] >= bestMinerals) {
                continue;
            }
            bestMinerals = minerals[j];
            bestNeighbor = static_cast<int>(j);
        }

        if (bestNeighbor < 0 || minerals[i] - bestMinerals < kMineralSlopeThreshold) {
            continue;
        }
        const int amount = (minerals[i] - bestMinerals) / 2;
        nextMinerals[i] -= amount;
        nextMinerals[static_cast<std::size_t>(bestNeighbor)] += amount;
    }

    // --- 7/8. Запись обратно: значения существующих компонентов правятся
    // напрямую, появление/исчезание WaterComponent — через очередь команд
    // (05_Entity.md, п.5: структурные изменения не мгновенны). ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        const auto entity = entities[i];
        if (entity == entt::null) {
            continue;
        }

        auto& soil = registry.get<SoilComponent>(entity);
        soil.moisture = std::clamp(nextMoisture[i], 0.0f, 1.0f);
        soil.minerals = std::max(0, nextMinerals[i]);
        registry.get<HeightComponent>(entity).height = nextTerrainHeight[i];

        const float depth = std::max(0.0f, nextWaterDepth[i]);
        const bool hadWater = waterDepth[i] > 0.0f;

        // Один порог на оба направления: вода есть ровно тогда, когда её
        // глубина выше kWaterMinDepth. Любая натёкшая глубина выше порога
        // записывается — неважно, была ли на тайле вода раньше (см.
        // комментарий у самой константы: пара порогов теряла массу на
        // кромке).
        if (depth > kWaterMinDepth) {
            if (hadWater) {
                registry.get<WaterComponent>(entity).depth = depth;
            } else {
                commands.enqueue([entity, depth](World& w) {
                    if (!w.registry().valid(entity) || w.registry().all_of<WaterComponent>(entity)) {
                        return;
                    }
                    w.registry().emplace<WaterComponent>(entity, WaterComponent{depth});
                });
            }
        } else if (hadWater) {
            commands.enqueue([entity](World& w) {
                if (w.registry().valid(entity)) {
                    w.registry().remove<WaterComponent>(entity);
                }
            });
        }
    }
}


// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
void appendHydrologyConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Hydrology";
    out.push_back({g, "kMoistureAdaptRate", kMoistureAdaptRate});
    out.push_back({g, "kWaterMinDepth", kWaterMinDepth});
    out.push_back({g, "kWaterRetention", kWaterRetention});
    out.push_back({g, "kPoolCells", static_cast<float>(kPoolCells)});
    out.push_back({g, "kVoidHeight", kVoidHeight});
    out.push_back({g, "kMineralMoistureThreshold", kMineralMoistureThreshold});
    out.push_back({g, "kMineralSlopeThreshold", static_cast<float>(kMineralSlopeThreshold)});
    out.push_back({g, "kRainDurationTicks", static_cast<float>(kRainDurationTicks)});
    out.push_back({g, "kRainDropsPerTick", static_cast<float>(kRainDropsPerTick)});
}

} // namespace goblins

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

// Расстояние до соседа по каждому из восьми направлений. Нужно там, где
// считается именно УКЛОН (падение уровня на единицу расстояния), а не
// просто разница высот: по диагонали до соседа дальше в sqrt(2) раз, и
// без этой поправки диагональные направления выигрывают конкуренцию за
// сток чаще, чем должны — вода расползается характерным квадратом
// вместо круга. Для BFS-дистанции до воды и для минералов (там всё
// решает не уклон, а сам факт соседства) поправка не нужна.
constexpr float kDiagonal = 1.41421356f;
constexpr float kDist8[8] = {kDiagonal, 1.0f, kDiagonal, 1.0f, 1.0f, kDiagonal, 1.0f, kDiagonal};

// Влажность: цель — общая с генерацией функция moistureTarget
// (core/Moisture.hpp), одна на весь проект. Здесь она не применяется
// разом, а служит целью, к которой влажность движется по
// kMoistureAdaptRate за тик: вода передвинулась — почва подсыхает или
// увлажняется постепенно, а не мгновенно.
constexpr float kMoistureAdaptRate = 0.01f;

// Утрамбованность: только размягчение, необратимо (без причины со стороны
// воды утрамбованность не меняется — 02_CorePrinciples.md, п.12). rockFloor —
// доля исходной утрамбованности, ниже которой каменистый участок не
// размягчается (камень остаётся твёрдым даже у самой воды).
constexpr float kCompactionRockFloor = 0.6f;
constexpr float kCompactionSoftenReach = 4.0f;
constexpr float kCompactionSoftenRate = 0.02f;

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

// Насколько круче склон — тем быстрее по нему течёт: фактическая доля
// выравнивания = waterFlowRate (свойство мира) + уклон * kWaterSlopeBoost,
// не больше 1. Без этого слагаемого скорость всюду одинаковая, и чтобы
// протолкнуть постоянный приток дальше по руслу, воде приходится
// накапливать большой стоячий перепад у истока — отсюда и "конус" вокруг
// источника вместо реки. Само число — форма закона, а не выбор конкретного
// мира: разным мирам оно ни разу не задавалось разным.
//
// Слагаемого "чем глубже, тем быстрее" (прежний kWaterDepthBoost = 2.0)
// здесь больше нет: при типичной глубине реки/пруда оно одно давало 2-6 и
// упирало долю в потолок 1.0 ВЕЗДЕ, где есть настоящая вода. То есть и
// waterFlowRate, и уклон переставали на что-либо влиять — ползунок Flow
// rate был мёртвым для всего, кроме тонкой кромки. Напор глубокой воды и
// так учтён: он весь в разнице поверхностей, от которой считается объём.
constexpr float kWaterSlopeBoost = 5.0f;

// Чем руководствуется вода, ВЫБИРАЯ соседа. Кандидатами остаются только
// те, у кого ниже ПОВЕРХНОСТЬ (высота + своя вода): течь вверх по уровню
// вода не может ни при каких весах, иначе пруд перестал бы выравниваться и
// начал бы сам себя накачивать. А вот среди кандидатов решает не один лишь
// перепад поверхности:
//
//   счёт = уклон поверхности
//        + уклон ДНА * kBedSlopeWeight
//        + kJoinWaterBonus, если у соседа уже стоит вода.
//
// Уклон дна с весом — это "вода идёт по руслу": промоина, уходящая вниз,
// перетягивает воду у ровного места, даже когда по уровню оба соседа
// почти одинаковы. Бонус за уже стоящую воду — "ручьи сливаются, а не
// текут рядом": на плоском месте поток скорее свернёт в существующее
// русло, чем расползётся по сухой земле новым языком.
//
// Оба — именно добавки к перепаду, а не замена ему: при заметной разнице
// уровней (десятые доли) она сама всё решает, и добавки лишь разводят
// почти равных кандидатов. Поэтому "старается", а не "всегда".
constexpr float kBedSlopeWeight = 2.0f;
constexpr float kJoinWaterBonus = 0.05f;

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

// Доля глубины, стекающая "за край карты" за тик — но только у тайлов на
// самой границе Области. Без этого края карты ведут себя как стенки:
// вода, которую течение толкает наружу, просто некуда девать, и она
// накапливается у границы. Доля (не абсолютная величина), поэтому убыль
// сама замедляется по мере обмеления.
constexpr float kEdgeDrainRate = 0.01f;

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
    const float waterSourceStrength = worldProperties.waterSourceStrength;
    const float waterEvaporationRate = worldProperties.waterEvaporationRate;
    const int rainIntervalTicks = worldProperties.rainIntervalTicks;
    const float rainAmount = worldProperties.rainAmount;
    const float waterFlowRate = worldProperties.waterFlowRate;
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
    std::vector<float> compaction(cellCount, 0.0f);
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
        compaction[i] = soil.compaction;
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

    // --- 3. Влажность и 4. утрамбованность: релаксация к цели ---
    std::vector<float> nextMoisture(moisture);
    std::vector<float> nextCompaction(compaction);
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (entities[i] == entt::null) {
            continue;
        }

        const float target = moistureTarget(distanceToWater[i], rockiness[i]);
        nextMoisture[i] = moisture[i] + (target - moisture[i]) * kMoistureAdaptRate;

        const float softenProximity =
            distanceToWater[i] >= 0 ? std::exp(-static_cast<float>(distanceToWater[i]) / kCompactionSoftenReach) : 0.0f;
        const float compactionFloor = rockiness[i] * kCompactionRockFloor;
        if (compaction[i] > compactionFloor) {
            nextCompaction[i] = compaction[i] - (compaction[i] - compactionFloor) * kCompactionSoftenRate * softenProximity;
        }
    }

    // --- 5. Течение + эрозия/осаждение. Три прохода по снимку, поэтому
    // порядок обхода клеток не влияет на результат.
    //
    //   5.1 намерения: каждая водная клетка выбирает ОДНОГО соседа и
    //       считает, сколько хотела бы ему отдать;
    //   5.2 приём: у каждого получателя входящий поток масштабируется так,
    //       чтобы его поверхность не поднялась выше самого низкого из
    //       отправителей;
    //   5.3 применение: вода, эрозия и осаждение — уже по итоговым объёмам.
    //
    // Второй проход и есть главное лекарство от ряби. Раньше ограничена
    // была только ОТДАЧА: клетка отдаёт половину разницы одному соседу —
    // для ПАРЫ это ровно выравнивание без перелёта. Но принимать клетка
    // может от всех восьми сразу, и каждый считал свою половину независимо
    // по одному и тому же снимку. Локальный минимум за тик получал до
    // восьми половин, его поверхность перелетала выше соседей, на следующем
    // тике вода шла обратно — шахматное дрожание по всей воде. ---
    std::vector<float> nextWaterDepth(waterDepth);
    std::vector<float> nextTerrainHeight(terrainHeight);

    const float kNoSender = std::numeric_limits<float>::infinity();
    std::vector<int> flowTarget(cellCount, -1);
    std::vector<float> flowAmount(cellCount, 0.0f);
    std::vector<float> inflow(cellCount, 0.0f);
    // Поверхность и дно самого низкого из отправителей в эту клетку —
    // потолок и для принимаемой воды (5.2), и для осаждаемой породы (5.3).
    std::vector<float> lowestSenderSurface(cellCount, kNoSender);
    std::vector<float> lowestSenderBed(cellCount, kNoSender);

    // --- 5.1 Намерения ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        // Течёт только вода выше удерживаемой почвой плёнки
        // (kWaterRetention). Уровень поверхности при этом считается от
        // ПОЛНОЙ глубины: удержанная вода никуда не делась, она просто не
        // течёт.
        const float mobileDepth = waterDepth[i] - kWaterRetention;
        if (mobileDepth <= 0.0f) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;
        const float surface = terrainHeight[i] + waterDepth[i];

        // Направление стока. Кто вообще может принять воду, решает
        // ПОВЕРХНОСТЬ (высота + своя вода): течь можно только туда, где
        // она ниже — вода не поднимается сама к себе, и на этом же держится
        // выравнивание прудов. Кого из подошедших выбрать, решает счёт
        // (kBedSlopeWeight/kJoinWaterBonus выше): вниз по руслу и к уже
        // стоящей воде — на плоском месте поток скорее свернёт в
        // существующее русло, чем расползётся по сухому новым языком.
        //
        // Всё делится на расстояние до соседа (а не берётся сырым
        // перепадом): иначе диагональный сосед с тем же падением
        // выигрывает наравне с ортогональным, хотя он дальше, и вода
        // расползается квадратом.
        //
        // Правило одно на любую воду — и на реку, и на пруд: "река" вообще
        // не понятие для симуляции, это просто вода, которой есть куда
        // стекать.
        int bestNeighbor = -1;
        float bestScore = 0.0f;
        float bestSlope = 0.0f;
        float bestNeighborSurface = surface;
        float bestDistance = 1.0f;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + kDx8[dir];
            const int ny = y + kDy8[dir];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                continue;
            }
            const std::size_t ni = index(nx, ny);
            const float neighborSurface = terrainHeight[ni] + waterDepth[ni];
            const float drop = surface - neighborSurface;
            // Единственное жёсткое условие: вверх по уровню воды не течём.
            if (drop <= 0.0f) {
                continue;
            }
            // Всё остальное — предпочтение (см. kBedSlopeWeight/
            // kJoinWaterBonus): вниз по руслу и к уже стоящей воде.
            const float dist = kDist8[dir];
            const float surfaceSlope = drop / dist;
            const float bedSlope = (terrainHeight[i] - terrainHeight[ni]) / dist;
            const float score = surfaceSlope + bedSlope * kBedSlopeWeight +
                                 (waterDepth[ni] > 0.0f ? kJoinWaterBonus : 0.0f);
            // bestNeighbor < 0 — первый подошедший кандидат берётся всегда:
            // счёт, в отличие от перепада, может быть и отрицательным (дно
            // круто идёт вверх, а поверхность чуть ниже), и сравнение с
            // нулём отбрасывало бы годных соседей.
            if (bestNeighbor < 0 || score > bestScore) {
                bestScore = score;
                // В скорость (rate ниже) идёт именно уклон ПОВЕРХНОСТИ: это
                // напор, который двигает воду, тогда как счёт выше — лишь
                // способ выбрать, куда именно.
                bestSlope = surfaceSlope;
                bestNeighborSurface = neighborSurface;
                bestDistance = dist;
                bestNeighbor = static_cast<int>(ni);
            }
        }

        if (bestNeighbor < 0) {
            continue;
        }
        const std::size_t j = static_cast<std::size_t>(bestNeighbor);
        const float diff = surface - bestNeighborSurface;
        // Чем круче склон, тем быстрее по нему течёт (kWaterSlopeBoost
        // выше). С постоянной скоростью, чтобы протолкнуть дальше
        // постоянный приток, воде приходилось копить у истока большой
        // стоячий перепад (доля ограничена, значит нужен большой diff) —
        // отсюда и "конус" вокруг источника. Потолок 1.0 и множитель 0.5
        // дают ровно выравнивание пары без перелёта.
        //
        // Делим на расстояние до соседа по той же причине, по какой на него
        // делится уклон при выборе: диагональный сосед дальше в sqrt(2)
        // раз. Без этого сосед выбирался по одной метрике, а объём считался
        // по другой — диагонали переносили столько же, сколько ортогонали,
        // и вода расползалась квадратом.
        const float rate = std::min(1.0f, waterFlowRate + bestSlope * kWaterSlopeBoost);
        const float amount = std::min(mobileDepth, diff * 0.5f) * rate / bestDistance;
        if (amount <= 0.0f) {
            continue;
        }

        flowTarget[i] = bestNeighbor;
        flowAmount[i] = amount;
        inflow[j] += amount;
        lowestSenderSurface[j] = std::min(lowestSenderSurface[j], surface);
        lowestSenderBed[j] = std::min(lowestSenderBed[j], terrainHeight[i]);
    }

    // --- 5.2 Приём: масштаб входящего потока ---
    // Получателю разрешено подняться максимум до поверхности САМОГО НИЗКОГО
    // из отправителей — выше неё вода в принципе не могла бы натечь, там
    // начался бы обратный сток. Считаем от снимка: собственный отток
    // получателя не учитываем, то есть оцениваем запас с запасом в меньшую
    // сторону — это и делает шаг заведомо неколебательным.
    std::vector<float> inflowScale(cellCount, 1.0f);
    for (std::size_t j = 0; j < cellCount; ++j) {
        if (inflow[j] <= 0.0f) {
            continue;
        }
        const float surface = terrainHeight[j] + waterDepth[j];
        const float allowed = std::max(0.0f, lowestSenderSurface[j] - surface);
        if (inflow[j] > allowed) {
            inflowScale[j] = allowed / inflow[j];
        }
    }

    // --- 5.3 Применение: вода, эрозия, осаждение ---
    // Эрозия и осаждение — две ветки одного закона, а не разные механизмы:
    //   дно клетки НЕ НИЖЕ соседского -> порода мешает течению, вымываем;
    //   дно соседа НИЖЕ -> впереди углубление, туда оседает нанос.
    // Сколько за тик суммарно вымыло, столько же и раздаётся по
    // углублениям (ниже). Раньше вымытое возвращалось в мир иначе: оно
    // сыпалось в восемь СЛУЧАЙНЫХ клеток карты, в том числе прямо в воду и
    // в русло, поднимая там дно, — отсюда бугры под водой и пороги в
    // старательно выглаженном при генерации русле.
    float totalEroded = 0.0f;
    std::vector<float> depositCapacity(cellCount, 0.0f);
    std::vector<bool> receivedWater(cellCount, false);
    float totalCapacity = 0.0f;

    for (std::size_t i = 0; i < cellCount; ++i) {
        if (flowTarget[i] < 0) {
            continue;
        }
        const std::size_t j = static_cast<std::size_t>(flowTarget[i]);
        const float amount = flowAmount[i] * inflowScale[j];
        if (amount <= 0.0f) {
            continue;
        }

        nextWaterDepth[i] -= amount;
        nextWaterDepth[j] += amount;
        // Нанос оседает только туда, куда вода этим тиком реально пришла, —
        // намерения из 5.1 для этого мало: приём мог быть отмасштабирован в
        // ноль (5.2), и тогда никакого потока, несущего породу, не было.
        receivedWater[j] = true;

        if (terrainHeight[i] <= terrainHeight[j]) {
            // Разная почва вымывается по-разному: каменистая и
            // утрамбованная сопротивляются размыву, рыхлая уходит легко.
            const float softness = (1.0f - rockiness[i]) * (1.0f - compaction[i]);

            // Потолок выемки. Без него клетка под постоянным источником
            // размывается каждый тик без остановки — высота уезжает в
            // минус, и появляется бездонная яма, никак не связанная с
            // рельефом вокруг. Ограничиваем глубину относительно соседа,
            // с которым клетка обменивается водой: ниже, чем на
            // maxErosionDepth под ним, размыть нельзя. Считаем от снимка
            // (terrainHeight), а не от накопителя, чтобы результат не
            // зависел от порядка обхода клеток — как и всё остальное в
            // этом шаге.
            const float erosionFloor = terrainHeight[j] - maxErosionDepth;
            const float allowedErosion = std::max(0.0f, terrainHeight[i] - erosionFloor);
            const float erosion = std::min(amount * soilErosionRate * softness, allowedErosion);

            nextTerrainHeight[i] -= erosion;
            totalEroded += erosion;
        }
    }

    // Ёмкость углублений: клетку разрешено поднять максимум до дна самого
    // низкого из отправителей — ровно "засыпать ступеньку вровень", и ни
    // крупицей выше. Тот же приём "самый низкий отправитель", что и у воды
    // выше: нанос не может создать новый бугор, а значит и новую рябь.
    for (std::size_t j = 0; j < cellCount; ++j) {
        if (!receivedWater[j] || entities[j] == entt::null) {
            continue;
        }
        const float capacity = std::max(0.0f, lowestSenderBed[j] - terrainHeight[j]);
        depositCapacity[j] = capacity;
        totalCapacity += capacity;
    }

    if (totalEroded > 0.0f && totalCapacity > 0.0f) {
        // Наноса больше, чем углублений, — излишку просто некуда осесть, и
        // он уходит с карты, как вода за край (kEdgeDrainRate ниже):
        // Область — открытый кусок мира, а не замкнутый сосуд, и требовать
        // от неё точного баланса почвы не за чем.
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
    // Все три величины — свойства мира (06_GameLoop.md, п.1a) и вместе
    // образуют баланс воды: сколько приносят источники и дожди, столько же
    // в равновесии должны уносить испарение и сток за край (5c ниже).
    // waterSourceStrength — АБСОЛЮТНАЯ величина, не множитель испарения:
    // раньше была множителем, и при маленьком испарении источник не мог
    // угнаться за собственным оттоком через течение выше — глубина
    // проседала почти до нуля независимо от того, насколько увеличивали
    // "силу". ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        if (waterDepth[i] > 0.0f) {
            nextWaterDepth[i] -= waterEvaporationRate;
        }
        if (isWaterSource[i]) {
            nextWaterDepth[i] += waterSourceStrength;
        }
    }

    // Дождь: раз в rainIntervalTicks тиков начинается дождь длиной
    // kRainDurationTicks, и каждый его тик роняет kRainDropsPerTick капель
    // по rainAmount глубины в случайные клетки. Идёт ли сейчас дождь и
    // куда именно падают капли, определяется одним лишь номером тика (плюс
    // seed мира), поэтому дожди воспроизводимы и одинаковы после любой
    // загрузки: собственного "погодного" состояния, которое пришлось бы
    // сохранять в файл мира, у мира нет.
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

    // --- 5c. Сток за край карты: у тайлов на самой границе Области —
    // доля глубины уходит "за карту" независимо от испарения/течения. Без
    // этого края ведут себя как стенки: течение толкает воду к краю, а
    // деваться ей там некуда, и она просто копится. Доля (не абсолютная
    // величина) — убыль сама замедляется по мере обмеления, "не быстро"
    // получается без отдельного потолка. Уходит только подвижная вода: то,
    // что удерживает почва (kWaterRetention), за край не утекает по той же
    // причине, по которой не течёт к соседям. ---
    for (std::size_t i = 0; i < cellCount; ++i) {
        const float mobileDepth = waterDepth[i] - kWaterRetention;
        if (mobileDepth <= 0.0f) {
            continue;
        }
        const int x = static_cast<int>(i) % width;
        const int y = static_cast<int>(i) / width;
        if (x == 0 || x == width - 1 || y == 0 || y == height - 1) {
            nextWaterDepth[i] -= mobileDepth * kEdgeDrainRate;
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
        soil.compaction = std::clamp(nextCompaction[i], 0.0f, 1.0f);
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
    out.push_back({g, "kCompactionRockFloor", kCompactionRockFloor});
    out.push_back({g, "kCompactionSoftenReach", kCompactionSoftenReach});
    out.push_back({g, "kCompactionSoftenRate", kCompactionSoftenRate});
    out.push_back({g, "kWaterMinDepth", kWaterMinDepth});
    out.push_back({g, "kWaterRetention", kWaterRetention});
    out.push_back({g, "kWaterSlopeBoost", kWaterSlopeBoost});
    out.push_back({g, "kBedSlopeWeight", kBedSlopeWeight});
    out.push_back({g, "kJoinWaterBonus", kJoinWaterBonus});
    out.push_back({g, "kEdgeDrainRate", kEdgeDrainRate});
    out.push_back({g, "kMineralMoistureThreshold", kMineralMoistureThreshold});
    out.push_back({g, "kMineralSlopeThreshold", static_cast<float>(kMineralSlopeThreshold)});
    out.push_back({g, "kRainDurationTicks", static_cast<float>(kRainDurationTicks)});
    out.push_back({g, "kRainDropsPerTick", static_cast<float>(kRainDropsPerTick)});
}

} // namespace goblins

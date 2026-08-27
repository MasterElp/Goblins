#include "core/systems/PlantSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Humus.hpp"
#include "core/Scale.hpp"
#include "core/Trees.hpp"
#include "core/components/HumusComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PlantGenomeComponent.hpp"
#include "core/components/PlantSpeciesComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SeedComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/TreeComponent.hpp"
#include "core/Berries.hpp"
#include "core/Trample.hpp"
#include "core/Store.hpp"
#include "core/components/StoreComponent.hpp"
#include "core/components/BuildingComponent.hpp"
#include "core/PlantKind.hpp"
#include "core/components/BerryComponent.hpp"
#include "core/components/BushComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/PlantGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Подсушивания клетки растением здесь больше нет, и это не оптимизация, а
// необходимость. Оно было половиной пары: трава тянула влажность вниз,
// HydrologySystem тянул её обратно к равновесию по близости воды, и куст
// вставал там, где эти две скорости сравнивались. Вторую половину пары
// отключили (релаксация влажности в HydrologySystem закомментирована), и
// оставшаяся половина превратилась в храповик: влажность мира могла
// только убывать, каждое растение навсегда портило свою клетку, луг
// высушивал карту под собой и вымирал целиком. Влажность теперь — то, чем
// её сделала генерация; вернётся релаксация — вернётся и подсушивание,
// но только вместе с ней.
//
// Вместе с подсушиванием ушёл и размер (kMinSizeShare): он делил между
// проростком и взрослым кустом ровно эту трату и больше ни на что не
// влиял.

// Единственная смерть травы не от старости и не от зубов: клетку залило
// глубже, чем переносит геном (waterTolerance). Счёт идёт, пока растение
// стоит под водой, и обнуляется, как только вода ушла, — то есть разлив
// выкашивает луг за десяток тиков, а пережитый разлив не помнится вовсе.
//
// Неблагополучия от сухости рядом больше нет. Оно копилось ниже половины
// обеспеченности влагой — а именно этот порог не пускает семя в клетку
// (kSeedMinSupply ниже, kSeedingMinSupply в GrassSeeding.cpp), так что
// расти сухому стрессу было почти негде, зато любая просадка влажности
// убивала весь луг разом. Сухая клетка теперь останавливает рост, и всё.
constexpr int kDrownStress = 100;

// Размножение. Семя может дать только достаточно выросшее растение, и
// каждое семя стоит родителю части роста — именно эта цена, а не отдельный
// "лимит потомков", не даёт траве залить мир за десяток тиков: после
// каждого семени растению нужно заново отрастить потраченное. Других
// условий нет: ни минералов, ни благополучия, ни запаса влаги.
constexpr int kSeedMinGrowth = 500;
// Цена семени в биомассе — заметно больше, чем "чуть-чуть": она должна
// сбрасывать растение НИЖЕ порога kSeedMinGrowth, иначе высеваться можно
// подряд каждый тик и скорость роста ни на что не влияет. С такой ценой
// частота потомства упирается ещё и в growthRate (нужно отрастить
// потраченное), то есть плодовитость приходится покупать дважды.
constexpr int kSeedGrowthCost = 450;
constexpr int kSeedlingGrowth = 20;

// Куда семя ложиться не станет: клетка суше, чем нужно потомку, чтобы
// расти хотя бы вполсилы. Тот же порог использует и первичное расселение
// (kSeedingMinSupply в GrassSeeding.cpp).
//
// Теперь это единственное, что оставляет на карте голые места. Пока трава
// копила стресс от сухости, сухие углы пустовали сами: семя туда
// попадало, но растение там не жило. Стресса от сухости больше нет —
// проросток в степи не погибнет, он просто будет расти черепашьим шагом и
// никогда не дорастёт до kSeedMinGrowth. Убрать порог значило бы затянуть
// травой всю карту, включая ту, где ей расти нечем.
constexpr int kSeedMinSupply = 500;

// Семя, оставленное в своей же клетке (когда ронять потомка некуда),
// лежит не вечно: сперва спит положенный геному срок
// (PlantGenomeComponent::seedDormancy), потом столько тиков сторожит
// клетку, а не дождавшись — гниёт, и его крупица уходит в перегной.
// Окно ожидания — общее для всех, а не черта генома: иначе плата за
// "долго лежащее семя" уходила бы в тот же бюджет, что и срок покоя, и
// два числа тянули бы одну и ту же верёвку с разных концов. Величина
// соразмерна жизни травы (сотни тиков): семя переживает родителя, но не
// несколько поколений подряд — семенной банк в мире есть, вечного
// семенного банка нет.
constexpr int kSeedWaitTicks = 400;

// --- Дерево ---
// Чем оно отличается от травы, объяснено в core/Trees.hpp; здесь только
// числа самого тика.

// С какой развитости дерево роняет семена. Выше травяного порога (500)
// намеренно: половина роста для дерева — это подрост, которому корней
// хватило впритык. Роняя семена, такой подрост расселял бы рощу на землю,
// которая его самого едва прокормила, и роща расползалась бы бледной
// тенью вместо того, чтобы стоять на богатом пятне.
constexpr int kTreeSeedMinGrowth = 700;

// Сколько крупиц должно лежать в земле вокруг клетки, чтобы туда легло
// семя дерева. Ровно столько, сколько нужно взрослому дереву: семя не
// ложится туда, где не выйдет дерева, — та же мысль, что и kSeedMinSupply
// у травы, только мерка своя.
//
// Это и есть то, что оставляет между рощами пустое место: на бедном пятне
// сумма под корнями не набирается никогда, а под уже стоящей рощей она
// выбрана самой рощей.
constexpr int kTreeSeedMinerals = kTreeMinerals;

// Сколько тиков семя дерева ждёт своего часа после срока покоя. На порядок
// больше травяного окна и по другой причине: травяное семя ждёт смерти
// своего родителя, а древесное — просвета в траве, то есть смены целого
// поколения луга под собой. Семя, лежащее меньше этого срока, до просвета
// почти никогда не доживало бы, и рощи не росли бы вовсе.
constexpr int kTreeSeedWaitTicks = 4000;

// Намерение посеять: собирается при обходе растений, исполняется после
// него. Отдельный шаг нужен потому, что на одну свободную клетку могут
// нацелиться сразу несколько соседей, и решать, кто её займёт, порядком
// обхода Entity нельзя — результат тика не должен зависеть от того, в
// каком порядке EnTT хранит растения (как и в HydrologySystem, где всё
// считается из снимка в накопители). Спор выигрывает наибольший
// priority — число, посчитанное из seed мира, номера тика и обеих клеток,
// то есть одинаковое при любом порядке обхода и разное у разных пар
// соседей.
struct SeedIntent {
    entt::entity parent;
    std::size_t targetCell;
    std::uint64_t priority;
    PlantGenomeComponent child;
    // Род потомка. Дерево не сажает проросток сразу: оно кладёт в клетку
    // семя, которое дождётся просвета в траве (см. kTreeSeedWaitTicks);
    // куст и трава сажают проросток. Разрешение спора за клетку при этом
    // одно на всех — спорят они за одно и то же место.
    PlantKind kind = PlantKind::Grass;
};

} // namespace

void PlantSystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    auto& registry = world.registry();
    const auto& worldProperties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    // Настройка мира целая (тысячные доли вложения), а расклад бюджета
    // черт дробный: перевод один раз здесь, на границе (core/Scale.hpp).
    const float mutationRate = static_cast<float>(worldProperties.plantMutationRate) / kFull;
    const int humusDecayPeriod = std::max(1, worldProperties.humusDecayPeriod);
    const auto plantSeed = static_cast<std::uint64_t>(worldProperties.plantRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& speciesLists = registry.get<const PlantSpeciesComponent>(world.worldEntity());

    // --- 1. Снимок тайлов: где почва, где вода, где уже стоит растение ---
    // Тот же приём, что и в HydrologySystem: сначала плоские массивы по
    // Области, потом расчёт — иначе на каждое соседство пришлось бы
    // перебирать список Entity клетки.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> terrain(cellCount, kNullEntity);
    // Глубина воды, а не флаг "вода есть": переносимая глубина у каждого
    // вида своя (PlantGenomeComponent::waterTolerance), поэтому решение
    // "тонет или нет" принимается для каждого растения отдельно, ниже.
    std::vector<int> waterAt(cellCount, 0);
    std::vector<unsigned char> occupied(cellCount, 0);
    // Почва СОСЕДНИХ клеток — именно снимком, снятым до того, как хоть
    // одно растение начало пить и есть. Растение правит почву только своей
    // клетки (на тайле оно одно), а вот выбирая, куда уронить семя, оно
    // смотрит на соседей — и если смотреть на живые компоненты, то видно
    // уже объеденную теми соседями почву, которых EnTT просто хранит
    // раньше в памяти. Порядок хранения — деталь реализации, а не событие
    // мира: он давал бы устойчивое преимущество одним растениям перед
    // другими, и это было бы не случайностью, а перекосом.
    std::vector<SoilComponent> soilAt(cellCount);

    auto terrainView = registry.view<PositionComponent, SoilComponent>();
    for (const auto entity : terrainView) {
        const auto& position = terrainView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        terrain[i] = entity;
        soilAt[i] = terrainView.get<SoilComponent>(entity);
        if (const auto* water = registry.try_get<const WaterComponent>(entity)) {
            waterAt[i] = water->depth;
        }
    }

    // Деревья отдельным слоем от occupied: для травы дерево — такой же
    // занятый тайл, как другая трава, а вот другому дереву оно запрещает
    // встать не только на свою клетку, но и на соседние (core/Trees.hpp).
    std::vector<unsigned char> treeAt(cellCount, 0);
    for (const auto entity : registry.view<PlantComponent, PositionComponent>()) {
        const auto& position = registry.get<const PositionComponent>(entity);
        if (world.area().inBounds(position.x, position.y)) {
            occupied[index(position.x, position.y)] = 1;
            if (registry.all_of<TreeComponent>(entity)) {
                treeAt[index(position.x, position.y)] = 1;
            }
        }
    }

    // Крупицы, лежащие в земле, суммой по прямоугольнику — за одно
    // сложение вместо обхода клеток. Дерево спрашивает эту сумму дважды за
    // тик (хватит ли корням прокормить его; годится ли клетка под семя), и
    // без готовой суммы каждый такой вопрос стоил бы обхода 5x5.
    //
    // Сумма снята в начале тика, как и soilAt: пока деревья тянут крупицы,
    // она слегка отстаёт от правды. Отставание не больше одной крупицы на
    // дерево за тик и никуда не копится — зато не зависит от того, в каком
    // порядке EnTT хранит деревья.
    std::vector<int> mineralsPrefix(static_cast<std::size_t>(width + 1) * (height + 1), 0);
    for (int y = 0; y < height; ++y) {
        int rowSum = 0;
        for (int x = 0; x < width; ++x) {
            rowSum += soilAt[index(x, y)].minerals;
            mineralsPrefix[static_cast<std::size_t>(y + 1) * (width + 1) + (x + 1)] =
                mineralsPrefix[static_cast<std::size_t>(y) * (width + 1) + (x + 1)] + rowSum;
        }
    }

    // Сколько крупиц лежит в земле под корнями дерева, стоящего в (x, y).
    auto rootMinerals = [&](int x, int y) {
        const int x0 = std::max(0, x - kTreeRootRadius);
        const int y0 = std::max(0, y - kTreeRootRadius);
        const int x1 = std::min(width - 1, x + kTreeRootRadius);
        const int y1 = std::min(height - 1, y + kTreeRootRadius);
        const auto at = [&](int px, int py) {
            return mineralsPrefix[static_cast<std::size_t>(py) * (width + 1) + px];
        };
        return at(x1 + 1, y1 + 1) - at(x0, y1 + 1) - at(x1 + 1, y0) + at(x0, y0);
    };

    // Стоит ли дерево вплотную к этой клетке (включая её саму).
    auto treeNear = [&](int x, int y) {
        for (int dy = -kTreeSpacing; dy <= kTreeSpacing; ++dy) {
            for (int dx = -kTreeSpacing; dx <= kTreeSpacing; ++dx) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (world.area().inBounds(nx, ny) && treeAt[index(nx, ny)] != 0) {
                    return true;
                }
            }
        }
        return false;
    };

    // Где уже лежит семя. Отдельно от occupied: семя не занимает клетку
    // для растения (оно лежит в земле под ним, обычно как раз под своим
    // родителем) — но второму семени места в клетке нет.
    std::vector<unsigned char> seeded(cellCount, 0);
    for (const auto entity : registry.view<SeedComponent, PositionComponent>()) {
        const auto& position = registry.get<const PositionComponent>(entity);
        if (world.area().inBounds(position.x, position.y)) {
            seeded[index(position.x, position.y)] = 1;
        }
    }

    // --- 2. Жизнь растений ---
    // Намерения посеять копятся здесь и исполняются ниже, после обхода
    // (см. SeedIntent): занятость клеток при выборе места берётся из
    // снимка выше и в ходе обхода не меняется, поэтому кто куда целится
    // не зависит от порядка обхода Entity.
    std::vector<SeedIntent> seedIntents;
    // Намерение корней взять крупицу из клетки. Тот же приём и по той же
    // причине, что у семян: корни соседних деревьев накрывают одни и те же
    // клетки, и кому достанется крупица, не должно решаться порядком, в
    // котором EnTT хранит деревья. Одна крупица с клетки за тик — больше
    // дереву за тик и не нужно (крупица это шестидесятая его роста, а
    // растёт оно на тысячные).
    struct RootIntent {
        entt::entity tree;
        std::size_t cell;
        std::uint64_t priority;
    };
    std::vector<RootIntent> rootIntents;
    auto plants = registry.view<PlantComponent, PlantGenomeComponent, PositionComponent>();
    for (const auto entity : plants) {
        auto& plant = plants.get<PlantComponent>(entity);
        const auto& genome = plants.get<PlantGenomeComponent>(entity);
        const auto& position = plants.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        if (terrain[i] == entt::null) {
            continue;
        }
        auto& soil = registry.get<SoilComponent>(terrain[i]);
        const PlantKind kind = plantKindOf(registry, entity);
        const bool isTree = kind == PlantKind::Tree;

        // Тонет растение или нет, решает не факт наличия воды, а её
        // глубина против переносимой этим геномом (water_tolerance).
        // Мокрая земля и мелкая лужа после дождя росту не мешают вовсе;
        // настоящее затопление — клетка непригодна (пригодность 0), и
        // дальше растение добьёт kDrownStress.
        const bool drowning = waterAt[i] > genome.waterTolerance;

        // Обеспеченность влагой — единственное, что решает, хорошо ли
        // растению на этом месте. Отдельной "пригодности почвы" по
        // каменистости и утоптанности больше нет: камень и так суше
        // (kRockMoistureReduction, core/Moisture.hpp), и голые горы
        // получаются от той же причины, что и всё остальное здесь.
        //
        // Запаса влаги в теле тоже нет — растение пьёт из клетки прямо
        // сейчас, а не живёт накопленным. Засуху переживает не тот, у кого
        // больше запас, а тот, кому изначально нужно меньше.
        const int supply =
            genome.moistureNeed > 0 ? std::min(kFull, soil.moisture * kFull / genome.moistureNeed) : kFull;

        // Рост: скорость генома, ослабленная обеспеченностью влагой. Больше
        // ничего: ни потолка по минералам, ни платы за место. Влага —
        // единственное, что решает, как быстро растёт трава, и сухая
        // клетка теперь только замедляет её, а не убивает.
        //
        // Скорость роста дробна по существу (скорость генома, ослабленная
        // влагой), поэтому неделящийся остаток остаётся в growthProgress —
        // целым, а не дробью. Без него растение при малой обеспеченности
        // не росло бы вовсе: целое деление отбросило бы весь прирост.
        // Утоптанность — вторая помеха росту после воды, и единственная,
        // которую в мир приносит чужое поведение, а не погода
        // (core/Trample.hpp). Тропа, на которой растёт трава, — не тропа, а
        // полоса на карте; поэтому связь, однажды убранная вместе с
        // habitatFit, вернулась вместе с теми, кто ходит.
        //
        // Дерева и куста это не касается: их корни глубже следа ноги, и
        // вытоптать рощу подошвами нельзя.
        const int trampled = kind == PlantKind::Grass ? soil.trampled : 0;
        const int vitality = drowning ? 0 : trampledGrowth(supply, trampled);
        plant.growthProgress += genome.growthRate * vitality;
        const int reach = std::min(kFull, plant.growth + plant.growthProgress / kFull);
        plant.growthProgress %= kFull;

        if (isTree) {
            // Дерево растёт ровно настолько, насколько его прокормили корни:
            // крупицы — не след роста, а его условие (core/Trees.hpp). Отсюда
            // и рощи: на бедном пятне дерево остаётся подростом и не роняет
            // семян, а роща на богатом выбирает своё пятно и перестаёт расти.
            const int needed = (kTreeMinerals * reach + kFull - 1) / kFull;
            if (plant.minerals < needed) {
                // Самая богатая клетка под корнями — по снимку начала тика,
                // а не по живой почве: увиденное не должно зависеть от того,
                // кто из деревьев считался раньше.
                std::size_t bestCell = cellCount;
                int bestMinerals = 0;
                for (int dy = -kTreeRootRadius; dy <= kTreeRootRadius; ++dy) {
                    for (int dx = -kTreeRootRadius; dx <= kTreeRootRadius; ++dx) {
                        const int nx = position.x + dx;
                        const int ny = position.y + dy;
                        if (!world.area().inBounds(nx, ny)) {
                            continue;
                        }
                        const std::size_t j = index(nx, ny);
                        if (soilAt[j].minerals > bestMinerals) {
                            bestMinerals = soilAt[j].minerals;
                            bestCell = j;
                        }
                    }
                }
                if (bestCell < cellCount) {
                    rootIntents.push_back(RootIntent{
                        entity, bestCell,
                        mixSeed(plantSeed, mixSeed(tick, bestCell * 1000003ull + static_cast<std::uint64_t>(i)))});
                }
            }
            // Потолок никогда не уменьшает уже достигнутый размер: отданная
            // семени доля роста не должна заставлять дерево съёжиться, а
            // выросшее дерево — усохнуть от того, что сосед выбрал землю.
            const int ceiling = plant.minerals * kFull / kTreeMinerals;
            plant.growth = std::min(reach, std::max(plant.growth, ceiling));
        } else {
            plant.growth = reach;

            // Трава набирает крупицы ПО МЕРЕ роста, а не растёт по мере
            // крупиц: сколько выросло, столько и просит, а нет их в клетке —
            // растёт дальше без них. Ровно эти крупицы потом уйдут в
            // травоядное или вернутся в клетку перегноем; больше
            // kPlantMinerals трава не берёт, иначе один старый куст выгреб бы
            // клетку и вернул бы туда гораздо больше, чем ему было нужно.
            const int wantedMinerals = kPlantMinerals * plant.growth / kFull;
            while (plant.minerals < wantedMinerals && soil.minerals > 0) {
                --soil.minerals;
                ++plant.minerals;
            }
        }

        // Ягоды на кусте. Закон один на всех, кто их спрашивает
        // (core/Berries.hpp): здесь они зреют, в GoblinSystem их рвут, в
        // наблюдателе рисуют.
        //
        // Зреют сроком со сдвигом по клетке — все кусты мира не должны
        // наливаться в один тик, — и стоят кусту крупицы из его же клетки.
        // Оттого ягодники и оказываются на богатой земле: на бедной куст
        // упирается в первую платную ягоду и дальше не зреет. Ни того, ни
        // другого специально никто не назначал.
        if (kind == PlantKind::Bush && berryDue(tick, i)) {
            if (auto* berries = registry.try_get<BerryComponent>(entity)) {
                ripenBerry(*berries, plant.growth, soil.minerals);
            }
        }

        // Под водой идёт счёт, на суше он обнуляется: пережитый разлив не
        // висит на растении и не убивает его вторым разливом полгода
        // спустя.
        plant.stress = drowning ? plant.stress + kDrownStress : 0;

        plant.age += 1;

        // Смерть от старости или от условий. Entity исчезает не сейчас, а
        // при разрешении очереди команд (05_Entity.md, п.5) — до конца
        // тика клетка считается занятой, и никто не посеет туда семя.
        if (plant.age >= genome.maxAge || plant.stress >= kFull) {
            commands.enqueue([entity, x = position.x, y = position.y](World& w) {
                if (!w.registry().valid(entity)) {
                    return;
                }
                // Сколько минералов уходит в перегной, читаем сейчас, а не
                // берём снятым при постановке команды: пока она ждала
                // очереди, тик успел доработать — например, растение
                // объело травоядное (AnimalSystem идёт следом) и часть
                // крупиц уже ушла в него. Со снятым числом эти крупицы
                // легли бы в перегной ВТОРОЙ раз.
                int minerals = 0;
                if (const auto* dying = w.registry().try_get<const PlantComponent>(entity)) {
                    minerals = dying->minerals;
                }
                depositHumus(w, x, y, minerals);
                w.despawn(entity);
            });
            continue;
        }

        // --- Размножение ---
        // Условий ровно два: дорос по возрасту и дорос по размеру. Минералы
        // тут ни при чём — они растению вообще ничего не запрещают; воды в
        // теле у него нет; неблагополучия, которое можно было бы спросить,
        // тоже больше нет.
        if (plant.age < genome.maturityAge ||
            plant.growth < (isTree ? kTreeSeedMinGrowth : kSeedMinGrowth)) {
            continue;
        }

        // Случайность собирается из seed мира, номера тика и координат
        // клетки: система не хранит генератор между тиками (05_Entity.md,
        // п.3), а исход не зависит от порядка обхода Entity. Второе — не
        // про повторяемость прогонов (мир недетерминирован,
        // 02_CorePrinciples.md, п.12a), а про то, что место в памяти не
        // должно быть причиной события в мире. Координаты, а не
        // идентификатор Entity, потому что при загрузке мира
        // идентификаторы выдаются заново, а координаты — те же.
        std::uint64_t random = mixSeed(plantSeed, mixSeed(tick, static_cast<std::uint64_t>(i)));
        // Шанс — тысячные за тик, ослабленные развитостью: недоросшее
        // растение сеет реже. Бросок целый (randomBelow), поэтому и
        // сравнивать есть с чем без всякого перевода в доли.
        if (static_cast<int>(randomBelow(random, static_cast<std::uint64_t>(kFull) * kFull)) >=
            genome.seedChance * plant.growth) {
            continue;
        }

        // Индекс вида — в СВОЁМ списке: у травы, у кустов и у деревьев свои
        // архетипы и свои таблицы черт (PlantSpeciesComponent).
        const auto& archetypes = kind == PlantKind::Tree    ? speciesLists.trees
                                  : kind == PlantKind::Bush ? speciesLists.bushes
                                                            : speciesLists.grasses;
        const auto& archetype =
            (genome.species >= 0 && static_cast<std::size_t>(genome.species) < archetypes.size())
                ? archetypes[static_cast<std::size_t>(genome.species)]
                : genome;
        const std::uint64_t childSeed = mixSeed(random, static_cast<std::uint64_t>(i));
        // Мутировать надо по СВОЕЙ таблице: перепутать — значит молча
        // получить куст, живущий триста тиков, или дерево-однолетку
        // (PlantGenetics.hpp).
        const PlantGenomeComponent child =
            kind == PlantKind::Tree   ? mutateTreeGenome(genome, archetype, mutationRate, childSeed)
            : kind == PlantKind::Bush ? mutateBushGenome(genome, archetype, mutationRate, childSeed)
                                      : mutateGenome(genome, archetype, mutationRate, childSeed);

        // --- Семя дерева: летит за несколько клеток и ложится ждать ---
        // Не проросток, а именно семя: под рощей всё занято травой, и
        // сажать сразу было бы некуда. Оно ляжет и дождётся, пока трава на
        // клетке отживёт своё (kTreeSeedWaitTicks).
        if (isTree) {
            constexpr int kTreeCandidateMax = (2 * kTreeSeedRange + 1) * (2 * kTreeSeedRange + 1);
            std::size_t treeCandidates[kTreeCandidateMax];
            int treeWeights[kTreeCandidateMax];
            int treeCandidateCount = 0;
            int treeTotalWeight = 0;
            for (int dy = -kTreeSeedRange; dy <= kTreeSeedRange; ++dy) {
                for (int dx = -kTreeSeedRange; dx <= kTreeSeedRange; ++dx) {
                    // Ближние клетки исключены самим правилом расстановки:
                    // вплотную к родителю дереву не встать (core/Trees.hpp).
                    if (std::max(std::abs(dx), std::abs(dy)) <= kTreeSpacing) {
                        continue;
                    }
                    const int nx = position.x + dx;
                    const int ny = position.y + dy;
                    if (!world.area().inBounds(nx, ny)) {
                        continue;
                    }
                    const std::size_t j = index(nx, ny);
                    // Занятость клетки травой здесь НЕ смотрим: семя за тем и
                    // ложится, чтобы дождаться просвета. А вот второе семя в
                    // клетку не влезет, стоящее дерево не пустит соседа, и
                    // вода потомку должна быть по силам.
                    if (seeded[j] != 0 || terrain[j] == entt::null || world.area().isBlocked(nx, ny) ||
                        waterAt[j] > child.waterTolerance || treeNear(nx, ny)) {
                        continue;
                    }
                    // Хватит ли земли вокруг, чтобы поднять взрослое дерево.
                    // Это и есть граница рощи: под уже стоящей рощей крупицы
                    // выбраны ею самой, а на бедном пятне их не было изначально.
                    const int supplyThere = rootMinerals(nx, ny);
                    if (supplyThere < kTreeSeedMinerals) {
                        continue;
                    }
                    treeCandidates[treeCandidateCount] = j;
                    treeWeights[treeCandidateCount] = supplyThere;
                    treeTotalWeight += supplyThere;
                    ++treeCandidateCount;
                }
            }
            if (treeCandidateCount == 0 || treeTotalWeight <= 0) {
                continue; // ронять некуда; бросок пропал, но и цена не уплачена
            }

            int treeRoll = static_cast<int>(randomBelow(random, static_cast<std::uint64_t>(treeTotalWeight)));
            int treePicked = treeCandidateCount - 1;
            for (int c = 0; c < treeCandidateCount; ++c) {
                treeRoll -= treeWeights[c];
                if (treeRoll < 0) {
                    treePicked = c;
                    break;
                }
            }
            const std::size_t treeTarget = treeCandidates[treePicked];
            seedIntents.push_back(SeedIntent{
                entity, treeTarget,
                mixSeed(plantSeed, mixSeed(tick, treeTarget * 1000003ull + static_cast<std::uint64_t>(i))), child,
                PlantKind::Tree});
            continue;
        }

        // Клетка для потомка выбирается среди восьми соседей: только те,
        // что существуют, свободны от другого растения, не залиты водой,
        // не заняты непроходимым объектом и подходят ИМЕННО ЭТОМУ геному
        // (у потомка он уже свой, слегка отличный от родительского).
        // Выбор — рулеткой по пригодности: чаще, но не всегда, семя
        // ложится в лучшую из соседних клеток, поэтому луг сам ползёт в
        // сторону подходящей почвы, но не превращается в ровный
        // наступающий фронт.
        std::size_t candidates[8];
        int weights[8];
        int candidateCount = 0;
        int totalWeight = 0;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = position.x + kDx8[dir];
            const int ny = position.y + kDy8[dir];
            if (!world.area().inBounds(nx, ny)) {
                continue;
            }
            const std::size_t j = index(nx, ny);
            // Переносимость воды берём у потомка: сеять есть смысл туда,
            // где сможет жить именно он, а его геном уже помутирован.
            if (occupied[j] != 0 || waterAt[j] > child.waterTolerance || terrain[j] == entt::null ||
                world.area().isBlocked(nx, ny)) {
                continue;
            }
            // Почва соседа — из снимка начала тика (см. soilAt выше), а
            // не живая: иначе увиденное зависело бы от того, кого EnTT
            // хранит раньше.
            //
            // Минералы клетки здесь не смотрим: безминеральная земля не
            // мешает прорасти, только расти дальше некуда, пока минералы не
            // появятся (течением или перегноем). Отказ по минералам тут был
            // бы ровно той путаницей, которую § "Размножение" выше уже
            // разрешил в другую сторону.
            const auto& targetSoil = soilAt[j];
            // Пригодность клетки для потомка — та же обеспеченность
            // влагой, по которой живёт и сам родитель (см. supply выше).
            // Отдельной функции пригодности почвы больше нет, и порогов
            // при ней тоже: клетка суше, чем нужно потомку в половину
            // силы, — не место для семени.
            const int targetSupply =
                child.moistureNeed > 0 ? std::min(kFull, targetSoil.moisture * kFull / child.moistureNeed) : kFull;
            if (targetSupply < kSeedMinSupply) {
                continue;
            }
            candidates[candidateCount] = j;
            weights[candidateCount] = targetSupply;
            totalWeight += targetSupply;
            ++candidateCount;
        }
        // --- Ронять некуда: семя остаётся в своей клетке ---
        // Все восемь соседей заняты, залиты или не годятся потомку. Семя
        // от этого не пропадает: оно ложится в ту же клетку, где стоит
        // родитель, и ждёт там своего часа (SeedComponent) — пока
        // родитель не умрёт от старости или не утонет. Именно так вид
        // держит занятое место за собой: на
        // заросшем лугу свободных клеток нет вовсе, и без семян
        // освободившуюся клетку занимал бы только тот, кто случайно
        // окажется рядом в нужный тик.
        if (candidateCount == 0 || totalWeight <= 0) {
            if (seeded[i] != 0) {
                continue; // одна клетка — одно семя; бросок пропал впустую, но и цена не уплачена
            }
            seeded[i] = 1;

            // Цена та же, что и у семени, брошенного в соседнюю клетку:
            // семя есть семя, и растению всё равно, куда оно упало. Минералы
            // в эту цену не входят — семя их не несёт, оно наберёт свои сам,
            // прорастая (см. §"Размножение" выше).
            plant.growth = std::max(0, plant.growth - kSeedGrowthCost);

            SeedComponent seed;

            commands.enqueue([child, seed, x = position.x, y = position.y](World& w) {
                const auto entity = w.registry().create();
                w.registry().emplace<SeedComponent>(entity, seed);
                w.registry().emplace<PlantGenomeComponent>(entity, child);
                w.place(entity, x, y);
            });
            continue;
        }

        int roll = static_cast<int>(randomBelow(random, static_cast<std::uint64_t>(totalWeight)));
        int picked = candidateCount - 1;
        for (int c = 0; c < candidateCount; ++c) {
            roll -= weights[c];
            if (roll < 0) {
                picked = c;
                break;
            }
        }
        const std::size_t targetCell = candidates[picked];

        // Сеем не здесь: клетку могли выбрать и другие соседи, а решать
        // спор порядком обхода нельзя (см. SeedIntent). Цену семени
        // родитель платит тоже там — проигравший спор не должен платить
        // за потомка, которого не будет.
        seedIntents.push_back(SeedIntent{
            entity, targetCell,
            mixSeed(plantSeed, mixSeed(tick, targetCell * 1000003ull + static_cast<std::uint64_t>(i))), child,
            kind});
    }

    // --- 2b. Разрешение споров за клетку и собственно посев ---
    // Сортируем по клетке, а внутри клетки — по убыванию priority, и
    // берём по одному намерению на клетку: результат один и тот же при
    // любом порядке обхода растений выше.
    std::sort(seedIntents.begin(), seedIntents.end(), [](const SeedIntent& a, const SeedIntent& b) {
        if (a.targetCell != b.targetCell) {
            return a.targetCell < b.targetCell;
        }
        return a.priority > b.priority;
    });

    for (std::size_t n = 0; n < seedIntents.size(); ++n) {
        if (n > 0 && seedIntents[n].targetCell == seedIntents[n - 1].targetCell) {
            continue; // клетку уже забрал сосед с большим priority
        }
        const SeedIntent& intent = seedIntents[n];
        const PlantGenomeComponent& child = intent.child;
        const int targetX = static_cast<int>(intent.targetCell % static_cast<std::size_t>(width));
        const int targetY = static_cast<int>(intent.targetCell / static_cast<std::size_t>(width));

        // Цена семени для родителя — только рост; минералы семя не несёт
        // (см. §"Размножение" выше).
        auto& parent = registry.get<PlantComponent>(intent.parent);
        parent.growth = std::max(0, parent.growth - kSeedGrowthCost);

        if (intent.kind == PlantKind::Tree) {
            // Дерево кладёт семя, а не проросток: клетка почти наверняка под
            // травой, и ждать просвета будет семя (см. §2c). Метка дерева
            // ложится на семя сразу — по ней оно и прорастёт деревом, а не
            // травой, и по ней же считается его срок ожидания.
            commands.enqueue([child, targetX, targetY](World& w) {
                const auto entity = w.registry().create();
                w.registry().emplace<SeedComponent>(entity, SeedComponent{});
                w.registry().emplace<PlantGenomeComponent>(entity, child);
                w.registry().emplace<TreeComponent>(entity);
                w.place(entity, targetX, targetY);
            });
            continue;
        }

        PlantComponent seedling;
        seedling.growth = kSeedlingGrowth;
        const bool bush = intent.kind == PlantKind::Bush;

        commands.enqueue([child, seedling, bush, targetX, targetY](World& w) {
            // Пока команда ждала своей очереди, тик успел доработать:
            // например, воду на клетке мог добавить HydrologySystem
            // (его команды встали в очередь раньше). Команда не имеет
            // права полагаться на состояние, снятое до её постановки, —
            // проверяем клетку заново.
            bool blocked = w.area().isBlocked(targetX, targetY);
            for (const auto tile : w.area().cellAt(targetX, targetY).entities) {
                if (w.registry().all_of<PlantComponent>(tile) || w.registry().all_of<WaterComponent>(tile)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                return; // семя не проросло — ронять было нечего, кроме места
            }

            const auto entity = w.registry().create();
            w.registry().emplace<PlantComponent>(entity, seedling);
            w.registry().emplace<PlantGenomeComponent>(entity, child);
            if (bush) {
                // Куст сеет проростком, как трава, а не семенем, как дерево:
                // купа растёт вширь от родителя, и ждать просвета ей незачем
                // — её и сажают туда, где просвет уже есть.
                w.registry().emplace<BushComponent>(entity);
                w.registry().emplace<BerryComponent>(entity);
            }
            w.place(entity, targetX, targetY);
        });
    }

    // --- 2b'. Кто из деревьев взял крупицу ---
    // Тот же способ, что и со спором за клетку: сортировка по клетке, внутри
    // клетки — по убыванию priority, и одна крупица достаётся первому.
    // Проигравший ничего не теряет и попробует на следующем тике.
    std::sort(rootIntents.begin(), rootIntents.end(), [](const RootIntent& a, const RootIntent& b) {
        if (a.cell != b.cell) {
            return a.cell < b.cell;
        }
        return a.priority > b.priority;
    });
    for (std::size_t n = 0; n < rootIntents.size(); ++n) {
        if (n > 0 && rootIntents[n].cell == rootIntents[n - 1].cell) {
            continue; // крупицу с этой клетки уже забрал сосед с большим priority
        }
        const RootIntent& intent = rootIntents[n];
        if (terrain[intent.cell] == entt::null) {
            continue;
        }
        auto& soil = registry.get<SoilComponent>(terrain[intent.cell]);
        auto* tree = registry.try_get<PlantComponent>(intent.tree);
        // Почву читаем живой, а не по снимку: снимок только выбирал, куда
        // тянуться, а есть ли там крупица на самом деле — вопрос этого мига.
        if (tree == nullptr || soil.minerals <= 0) {
            continue;
        }
        --soil.minerals;
        ++tree->minerals;
    }

    // --- 2c. Семена: покой, прорастание, гниение ---
    // Семя ничего не делает: не растёт, не пьёт, не тянет минералы из
    // почвы и не подлежит объеданию (у него нет PlantComponent). Оно
    // только считает тики и смотрит, не освободилась ли клетка.
    auto seeds = registry.view<SeedComponent, PlantGenomeComponent, PositionComponent>();
    for (const auto entity : seeds) {
        auto& seed = seeds.get<SeedComponent>(entity);
        const auto& genome = seeds.get<PlantGenomeComponent>(entity);
        const auto& position = seeds.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);

        // Метка дерева лежит на семени с рождения: по ней оно и прорастёт
        // деревом, и ждёт оно дольше — не смерти родителя, а просвета в
        // траве (см. kTreeSeedWaitTicks).
        const bool isTreeSeed = registry.all_of<TreeComponent>(entity);
        const int waitTicks = isTreeSeed ? kTreeSeedWaitTicks : kSeedWaitTicks;

        seed.age += 1;

        // Не дождалось — семя просто исчезает. Ему нечего вернуть почве:
        // оно не несёт минералов (см. SeedComponent), а из перегноя они
        // возвращаются только тем, чем растение при жизни успело обрасти.
        if (seed.age >= genome.seedDormancy + waitTicks) {
            commands.enqueue([entity](World& w) {
                if (!w.registry().valid(entity)) {
                    return;
                }
                w.despawn(entity);
            });
            continue;
        }

        if (seed.age < genome.seedDormancy) {
            continue; // ещё спит: срок покоя — черта генома (seed_dormancy)
        }

        // Клетка ещё занята растением (обычно тем самым родителем) — семя
        // просто ждёт дальше. Растение, умершее на этом же тике, в снимке
        // occupied всё ещё стоит: его Entity исчезнет только при
        // разрешении очереди команд, поэтому семя займёт клетку тиком
        // позже, а не в тот же миг.
        if (occupied[i] != 0 || waterAt[i] > genome.waterTolerance || terrain[i] == entt::null ||
            world.area().isBlocked(position.x, position.y)) {
            continue;
        }
        // Пока семя лежало, рядом могло подняться дерево: вплотную к нему не
        // встают (core/Trees.hpp), и семя продолжает ждать — места ему хватит
        // ровно тогда, когда сосед упадёт.
        if (isTreeSeed && treeNear(position.x, position.y)) {
            continue;
        }

        PlantComponent seedling;
        seedling.growth = kSeedlingGrowth;

        commands.enqueue([entity, genome, seedling, isTreeSeed, x = position.x, y = position.y](World& w) {
            if (!w.registry().valid(entity)) {
                return;
            }
            // Клетку могли занять, пока команда ждала очереди: например,
            // семя соседнего растения легло сюда тем же тиком (его команда
            // встала в очередь раньше). Проверяем заново — иначе на клетке
            // оказалось бы два растения.
            bool blocked = w.area().isBlocked(x, y);
            for (const auto tile : w.area().cellAt(x, y).entities) {
                if (w.registry().all_of<PlantComponent>(tile)) {
                    blocked = true;
                    break;
                }
                const auto* water = w.registry().try_get<const WaterComponent>(tile);
                if (water != nullptr && water->depth > genome.waterTolerance) {
                    blocked = true;
                    break;
                }
            }
            // Соседнее дерево могло встать этим же тиком (его команда встала
            // в очередь раньше) — расстановку проверяем заново, иначе два
            // дерева оказались бы вплотную.
            if (!blocked && isTreeSeed) {
                for (int dy = -kTreeSpacing; dy <= kTreeSpacing && !blocked; ++dy) {
                    for (int dx = -kTreeSpacing; dx <= kTreeSpacing && !blocked; ++dx) {
                        if (!w.area().inBounds(x + dx, y + dy)) {
                            continue;
                        }
                        for (const auto tile : w.area().cellAt(x + dx, y + dy).entities) {
                            if (w.registry().all_of<PlantComponent, TreeComponent>(tile)) {
                                blocked = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (blocked) {
                return; // семя остаётся лежать и попробует на следующем тике
            }

            // Семя не порождает растение, а САМО им становится: тот же
            // Entity, тот же геном, то же место — меняется лишь набор
            // компонентов (02_CorePrinciples.md, п.3). Отдельная сущность
            // "проросток" миру не нужна.
            w.registry().emplace<PlantComponent>(entity, seedling);
            w.registry().remove<SeedComponent>(entity);
        });
    }

    // --- 3. Перегной: разложение и возврат минералов в почву ---
    // Ровно те крупицы, что растение при жизни вынуло из этой клетки,
    // возвращаются в неё же — по одной раз в humusDecayPeriod тиков. Пока
    // перегной лежит, клетка постепенно становится плодороднее; когда
    // возвращать нечего, компонент снимается (02_CorePrinciples.md, п.3:
    // нет перегноя — нет и компонента).
    //
    // Срок, а не дробная скорость: "0.02 крупицы за тик" требовало поля
    // pending в самом перегное — только затем, чтобы дробь дожила до
    // целого. Отсчёт сдвинут на номер тайла, иначе весь перегной мира
    // отдавал бы свою крупицу одним и тем же тиком, а между этими тиками
    // почва не менялась бы вовсе.
    // Куча принесённой еды гниёт здесь же, и это не соседство ради удобства.
    // Гниль уходит в перегной той же клетки: ягода -> куча -> перегной ->
    // минералы в почву, тот же круг, которым возвращается в мир падаль.
    //
    // Место выбрано так, а не в GoblinSystem, по причине, которую иначе
    // заметить было бы нечем: та система возвращается сразу, если гоблинов в
    // мире не осталось, — и куча, набранная последним из них, лежала бы
    // нетленной до конца времён. Земля же гниёт независимо от того, есть ли
    // кому на неё смотреть.
    auto storeView = registry.view<StoreComponent, PositionComponent>();
    for (const auto entity : storeView) {
        auto& store = storeView.get<StoreComponent>(entity);
        const auto& storePosition = storeView.get<PositionComponent>(entity);
        // Постройки на этой же клетке берегут запас (core/Store.hpp): навес
        // вдвое, подстилка ещё вдвое. Склад в мире не заведён отдельной
        // вещью — он и есть навес, поставленный над кучей.
        const auto* shelter = registry.try_get<const BuildingComponent>(entity);
        const int canopy = shelter != nullptr ? shelter->canopy : 0;
        const int bedding = shelter != nullptr ? shelter->bedding : 0;
        const auto storeCell = static_cast<std::uint64_t>(index(storePosition.x, storePosition.y));
        if (store.food > 0 && storeRotDue(tick, storeCell, canopy, bedding)) {
            // Гниёт доля еды, а крупицы уходят с ней той же долей
            // (core/Portion.hpp) — вещество не пропадает, оно переезжает.
            const Portion rotted = takeFromStore(store, kStoreRot);
            if (rotted.minerals > 0) {
                commands.enqueue([x = storePosition.x, y = storePosition.y, minerals = rotted.minerals](World& w) {
                    depositHumus(w, x, y, minerals);
                });
            }
        }
        if (store.food <= 0) {
            commands.enqueue([entity](World& w) {
                // Проверяем заново: пока команда ждала очереди, на эту клетку
                // могли положить новую горсть (GoblinSystem идёт позже), и
                // снять компонент сейчас значило бы выбросить её из мира.
                auto* storeComponent = w.registry().try_get<StoreComponent>(entity);
                if (storeComponent != nullptr && storeComponent->food <= 0) {
                    w.registry().remove<StoreComponent>(entity);
                }
            });
        }
    }

    auto humusView = registry.view<HumusComponent, SoilComponent, PositionComponent>();
    for (const auto entity : humusView) {
        auto& humus = humusView.get<HumusComponent>(entity);
        auto& soil = humusView.get<SoilComponent>(entity);
        const auto& humusPosition = humusView.get<PositionComponent>(entity);

        const auto tile = static_cast<std::uint64_t>(index(humusPosition.x, humusPosition.y));
        if (humus.minerals > 0 && (tick + tile) % static_cast<std::uint64_t>(humusDecayPeriod) == 0) {
            --humus.minerals;
            ++soil.minerals;
        }

        if (humus.minerals <= 0) {
            commands.enqueue([entity](World& w) {
                // Проверяем заново, а не полагаемся на снятое выше
                // состояние: между постановкой команды и её выполнением
                // на этом же тайле могло умереть растение (команда смерти
                // встала в очередь раньше), и в опустевший перегной уже
                // легли его минералы — снять компонент сейчас значило бы
                // выбросить их из мира.
                auto* humusComponent = w.registry().try_get<HumusComponent>(entity);
                if (humusComponent != nullptr && humusComponent->minerals <= 0) {
                    w.registry().remove<HumusComponent>(entity);
                }
            });
        }
    }
}


// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
void appendPlantSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Plants (tick)";
    out.push_back({g, "kDrownStress", kDrownStress});
    out.push_back({g, "kSeedMinGrowth", kSeedMinGrowth});
    out.push_back({g, "kSeedGrowthCost", kSeedGrowthCost});
    out.push_back({g, "kSeedlingGrowth", kSeedlingGrowth});
    out.push_back({g, "kSeedMinSupply", kSeedMinSupply});
    out.push_back({g, "kSeedWaitTicks", kSeedWaitTicks});
    out.push_back({g, "kTreeSeedMinGrowth", kTreeSeedMinGrowth});
    out.push_back({g, "kTreeSeedMinerals", kTreeSeedMinerals});
    out.push_back({g, "kTreeSeedWaitTicks", kTreeSeedWaitTicks});
    out.push_back({g, "kTreeMinerals", kTreeMinerals});
    out.push_back({g, "kTreeRootRadius", kTreeRootRadius});
    out.push_back({g, "kTreeSpacing", kTreeSpacing});
    out.push_back({g, "kTreeSeedRange", kTreeSeedRange});

    // Ягоды — своей группой (core/Berries.hpp): эти числа решают, стоит ли
    // ходить к ягоднику, и смотреть на них надо рядом друг с другом, а не
    // вперемешку со сроками семян.
    constexpr const char* b = "Plants (berries)";
    out.push_back({b, "kBerryMass", static_cast<float>(kBerryMass)});
    out.push_back({b, "kBerryMax", static_cast<float>(kBerryMax)});
    out.push_back({b, "kBerryMaturity", static_cast<float>(kBerryMaturity)});
    out.push_back({b, "kBerryPeriod", static_cast<float>(kBerryPeriod)});
    out.push_back({b, "kBerriesPerGrain", static_cast<float>(kBerriesPerGrain)});

    // Гниение кучи — здесь же, потому что здесь оно и происходит: куча лежит
    // на земле рядом с перегноем и уходит в него же (core/Store.hpp).
    out.push_back({b, "kStoreRot", static_cast<float>(kStoreRot)});
}

} // namespace goblins

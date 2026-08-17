#include "core/systems/AnimalSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Humus.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/AnimalSpeciesComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/DesireComponent.hpp"
#include "core/components/HerbivoreComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/Diagnostics.hpp"

namespace goblins {

namespace {

constexpr int kDx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kDy8[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

// Насколько маленькое животное "меньше ест": и расход на жизнь, и укус, и
// цена шага, и скорость пищеварения, и сила удара умножаются на размер (от
// kMinSizeShare у новорождённого до 1 у взрослого) — тот же приём, что у
// растений. Без него детёныш объедал бы луг наравне со взрослым и почти
// никогда не выживал бы на бедной земле.
constexpr float kMinSizeShare = 0.3f;

// Сколько энергии даёт единица биомассы съеденного (до того, как её
// умножат на digestion генома). Число связывает две шкалы, которые иначе не
// сопоставимы: развитость растения живёт в [0, 1], а расход животного — в
// единицах за тик. Оно же переводит и мясо: биомасса травы и биомасса туши
// меряются одной шкалой, поэтому травоядное и хищник считают энергию
// одинаково, а разница между диетами остаётся в том, ГДЕ её взять, а не в
// том, сколько её в куске.
constexpr float kEnergyPerBiomass = 40.0f;

// Сколько воды животное получает из самой травы: трава сочная, и часть
// жажды утоляется кормёжкой. Множитель к влаге, накопленной растением
// (PlantComponent::moisture), в пересчёте на съеденную долю куста.
constexpr float kWaterPerBiomass = 6.0f;

// То же для мяса: в туше есть кровь, и наевшийся хищник долго может не
// искать воду. Своей влаги падаль не хранит (у неё нет поля, откуда её
// брать), поэтому вода считается прямо от съеденной биомассы.
//
// Число заметное, а не символическое: хищник уходит за добычей далеко от
// воды, и если бы поить его могла только река, он гиб бы от жажды посреди
// охоты — на первых прогонах именно так и выходило, хищники умирали с
// полным брюхом и пустой флягой.
constexpr float kWaterPerMeat = 6.0f;

// Цена одного шага в энергии (взрослому). Вместе со скоростью из генома это
// и есть плата за быстроту: быстрое животное делает больше шагов за тик,
// значит и тратит больше — скорость покупается не только бюджетом
// преимуществ, но и обменом веществ.
//
// Но плата не должна превосходить саму жизнь. При вдвое большем значении
// шаг стоил дороже, чем существование (energy_upkeep), и поиск еды
// обходился дороже найденного: хищник, обходящий свой участок, сжигал две
// своих ёмкости за тысячу тиков и умирал раньше, чем находил добычу. При
// вдвое меньшем — наоборот, ходьба переставала что-либо стоить, и стадо,
// которому корм давался даром, за тысячу тиков объедало весь мир.
constexpr float kStepEnergy = 0.25f;

// Водопой: сколько единиц воды животное выпивает за тик и сколько глубины
// это снимает с клетки. Второе число мало намеренно: стадо не должно
// выпивать реку — оно и не выпивает, но след в мире оставляет, и вода
// приходит не из ниоткуда.
constexpr float kDrinkRate = 4.0f;
constexpr float kDrinkDepthPerUnit = 0.002f;

// Ниже этой развитости куст объедать нечего, ниже этого мяса — нечего
// глодать: животное просто не считает такое едой. Иначе стадо паслось бы на
// голых проростках, не давая лугу отрасти, а хищник сидел бы у обглоданных
// костей вместо охоты.
constexpr float kMinBiteGrowth = 0.05f;
constexpr float kMinBiteMeat = 0.05f;

// Сколько стресса получает растение за единицу съеденной развитости. Именно
// так объедание убивает траву — не напрямую, а через её собственный закон
// смерти от условий (PlantSystem): куст, который скусывают снова и снова,
// не успевает отрасти и в конце концов погибает, а редко потревоженный
// отходит.
constexpr float kGrazeStress = 0.4f;

// Пищеварение: какая доля крупицы за тик проходит через взрослое животное
// в навоз. Отсюда и необходимость есть постоянно: белок не лежит в теле
// вечно. Навоз выпадает не по крупице, а порциями в kDungDrop крупиц —
// это навоз, а не равномерная плёнка по всему маршруту.
constexpr float kDungRate = 0.004f;
constexpr int kDungDrop = 2;

// Стресс. Голод убивает примерно за 1/kStarvationStress тиков, жажда
// быстрее — без воды живут меньше, чем без еды. Восстановление медленнее
// накопления: пережитая бескормица не забывается мгновенно.
constexpr float kStarvationStress = 0.02f;
constexpr float kDehydrationStress = 0.03f;
constexpr float kStressRelief = 0.01f;

// Раны затягиваются у того, кому есть чем: голодное животное не заживает.
// Медленно — чтобы уйти от хищника раненым значило ещё не спастись.
constexpr float kHealRate = 0.002f;

// Желания. Ниже kDesireFloor желание никуда не гонит — животное считается
// довольным и просто бродит. kDesireSwitch — насколько сильнее должно быть
// другое желание, чтобы перебить уже выбранное: без этого запаса животное с
// почти равными голодом и жаждой каждый тик разворачивалось бы и не дошло
// бы ни до травы, ни до воды.
constexpr float kDesireFloor = 0.35f;
constexpr float kDesireSwitch = 0.15f;

// Размножение. Желание пары копится только у взрослого, доросшего и не
// бедствующего животного (kCalmNeed — предел голода и жажды, при котором
// ещё до того), а идти искать партнёра оно начинает с kMateDesire.
//
// kCalmNeed — это "не бедствует", а не "сыто по горло". Разница решающая:
// голод и жажда считаются долей от СОБСТВЕННОЙ ёмкости, а хищник живёт
// большими запасами и редкой добычей, поэтому большую часть жизни проводит
// ниже половины запаса. При строгом пороге он оказывался вечно "голодным" и
// не размножался вовсе — на восемь тысяч тиков приходилось одно рождение
// при одиннадцати смертях, и хищники вымирали в мире, полном добычи. Порог
// отделяет бедствие от обычной жизни, а не сытость от несытости; само
// бедствие проверяется отдельно и прямо — по накопленному стрессу.
constexpr float kBreedingGrowth = 0.9f;
constexpr float kCalmNeed = 0.75f;
constexpr float kMateDesire = 0.6f;

// Цена потомства. Мать отдаёт детёнышу долю своих запасов и крупицу белка —
// ровно как растение отдаёт семени часть влаги и крупицу минералов; отец
// платит только ухаживанием (энергия тратится, но никуда не переходит).
// Именно эта цена, а не отдельный "лимит поголовья", и сдерживает
// размножение: после родов матери нужно заново отъедаться.
constexpr float kBirthEnergyShare = 0.45f;
constexpr float kBirthWaterShare = 0.3f;
constexpr float kCourtshipEnergyShare = 0.15f;
constexpr float kNewbornGrowth = 0.08f;

// Насколько один проход животного уплотняет почву под ногами. Черта
// "терпимость к утоптанности" (PlantGenomeComponent::compactionTolerance)
// была куплена растениями за бюджет ещё до появления животных и до сих пор
// ничего не значила: топтать почву было некому (docs/08_Plants.md, п.9).
// Теперь есть кому — тропа к водопою становится твёрдой, и трава на ней
// растёт хуже.
constexpr float kTrampleRate = 0.004f;

// С какой вероятностью ничего не желающее животное всё-таки делает шаг.
// Постоянно бродящее стадо выглядит нервным и зря жжёт энергию; полностью
// неподвижное — мёртвым.
constexpr float kWanderChance = 0.25f;

// Сколько тиков животное держит одно направление поиска, когда желаемого не
// видно. Это не украшение походки, а единственный способ найти что-то за
// пределами своей видимости: случайный шаг в случайную сторону уводит от
// исходной точки как корень из числа шагов, а прямая ходьба — линейно, то
// есть на порядок дальше за то же время.
//
// Без этого поголовье, разбросанное по большой карте, вымирало не от голода
// и не от зубов, а от одиночества: двум последним хищникам, чтобы принести
// потомство, нужно встретиться, а случайное блуждание сводило их вместе
// примерно никогда.
//
// Направление берётся из постоянного идентификатора и номера "отрезка"
// (tick / kRoamTicks): системе не нужно ничего помнить между тиками
// (05_Entity.md, п.3), а животное всё равно идёт в одну сторону, пока
// отрезок не сменится.
constexpr std::uint64_t kRoamTicks = 40;
// Как далеко впереди ставится воображаемая цель отрезка. Число значения не
// имеет — важно лишь, что она заведомо дальше соседних клеток, поэтому шаг
// выбирается в её сторону.
constexpr int kRoamReach = 8;

// --- Хищничество ---

// Сколько мяса остаётся от взрослого животного. В единицах биомассы, тех
// же, что развитость растения: туша — это десяток кустов травы разом,
// поэтому охота и окупается. От детёныша остаётся меньше — мясо считается
// от размера.
//
// Число не косметическое, а решающее: одна добыча должна кормить хищника
// заметно дольше, чем длится охота на следующую. При втрое меньшем значении
// хищники вымирали за первую тысячу тиков — они успевали загнать жертву, но
// не успевали окупить погоню, и весь их род держался на том, чтобы ни разу
// не промахнуться.
constexpr float kMeatPerSize = 8.0f;

// Как быстро падаль пропадает сама. Несъеденная туша не лежит вечно: она
// гниёт, и накопленный зверем белок доходит до почвы перегноем, просто
// медленнее, чем через чей-то желудок.
//
// Гниёт быстро — тушу целиком за несколько сотен тиков. При медленном
// гниении мир получал "банк падали": когда стадо, объевшее луг, вымирало
// разом от голода, сотни туш лежали тысячи тиков и кормили хищников
// вшестеро против того, что могла прокормить живая добыча. Расплодившись на
// падали, хищники доедали уцелевшее стадо, и мир пустел совсем. Мясо должно
// портиться — иначе однажды случившийся мор кормит вечно.
constexpr float kCarcassRot = 0.02f;

// С какого голода хищник выходит на охоту. Порог выше общего kDesireFloor
// намеренно: слегка проголодавшийся зверь подберёт падаль, если она рядом,
// но гнаться за живой добычей не станет. Сытый хищник, пропускающий добычу
// мимо, — не поблажка стаду, а единственное, что вообще удерживает его
// численность: пока хищники убивали при любом голоде, они выбивали стадо
// подчистую и вымирали следом, сколько ни крути прочие числа.
constexpr float kHuntHunger = 0.35f;

// Дотянуться зубами можно до своей клетки и до соседней. Не только до
// своей: жертва убегает каждый тик, и хищнику, который обязан встать ровно
// на её клетку, доставалась бы она только по случайности — охота
// выродилась бы в лотерею.
constexpr int kAttackReach = 1;

// --- Намерения ---
// Собираются при обходе животных и исполняются после него. Отдельный шаг
// нужен там же, где и в PlantSystem: на один куст, одну тушу и один
// водопой могут претендовать сразу несколько животных, и решать спор
// порядком обхода Entity нельзя (04_WorldModel.md, п.8). Спор делится
// долями: если желаемого меньше, чем просят, каждый получает свою часть —
// исход не зависит ни от порядка обхода, ни от того, кого EnTT хранит
// раньше.
struct ShareIntent {
    std::size_t cell = 0;      // клетка с кустом, тушей или водой
    int animal = 0;            // индекс в снимке животных
    std::uint64_t id = 0;      // постоянный идентификатор — им сортируем
    float want = 0.0f;
};

struct StepIntent {
    int animal = 0;
    int x = 0;
    int y = 0;
};

struct MateIntent {
    std::size_t cell = 0;
    int animal = 0;
    std::uint64_t id = 0;
    int species = 0;
    bool predator = false;
    Sex sex = Sex::Female;
};

// Удар. Урон складывается, а не делится: двое хищников, вцепившихся в одну
// жертву, наносят вдвое больше — здесь делить нечего, и порядок обхода на
// сумму не влияет.
struct AttackIntent {
    int prey = 0;
    float damage = 0.0f;
};

// Живое животное в снимке этого тика. Указатели на компоненты держать
// безопасно: за время обхода систем структура хранилища не меняется —
// создание и удаление Entity идёт только через очередь команд
// (05_Entity.md, п.5).
struct Animal {
    entt::entity entity = entt::null;
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    bool predator = false;
    AnimalComponent* state = nullptr;
    const AnimalGenomeComponent* genome = nullptr;
    DesireComponent* desire = nullptr;
};

bool sortByCellThenId(const ShareIntent& a, const ShareIntent& b) {
    if (a.cell != b.cell) {
        return a.cell < b.cell;
    }
    return a.id < b.id;
}

// Какое желание сейчас гонит животное. Инерция (kDesireSwitch) — часть
// закона, а не сглаживание: желание, за которым уже пошли, держится, пока
// другое не перевесит его с заметным запасом. Страх инерции не подчиняется
// в одну сторону — он и так пересчитывается каждый тик и пропадает вместе
// с опасностью.
Desire chooseDesire(const DesireComponent& desire, bool readyToMate) {
    const float mating = readyToMate && desire.mating >= kMateDesire ? desire.mating : 0.0f;

    Desire best = Desire::Idle;
    float bestUrgency = kDesireFloor;
    if (desire.hunger >= bestUrgency) {
        best = Desire::Food;
        bestUrgency = desire.hunger;
    }
    if (desire.thirst >= bestUrgency) {
        best = Desire::Water;
        bestUrgency = desire.thirst;
    }
    if (mating >= bestUrgency) {
        best = Desire::Mate;
        bestUrgency = mating;
    }
    // Страх проверяется последним и потому при равенстве побеждает: сытость
    // подождёт, зубы — нет.
    if (desire.fear >= bestUrgency) {
        best = Desire::Flee;
        bestUrgency = desire.fear;
    }

    float currentUrgency = 0.0f;
    switch (desire.current) {
        case Desire::Food: currentUrgency = desire.hunger; break;
        case Desire::Water: currentUrgency = desire.thirst; break;
        case Desire::Mate: currentUrgency = mating; break;
        case Desire::Flee: currentUrgency = desire.fear; break;
        case Desire::Idle: break;
    }
    if (desire.current != Desire::Idle && currentUrgency >= kDesireFloor &&
        bestUrgency < currentUrgency + kDesireSwitch) {
        return desire.current;
    }
    return best;
}

// Падаль ложится на терраформирующий Entity тайла, рядом с почвой, водой и
// перегноем (см. CarcassComponent). Если туша там уже лежит, мясо и белок
// просто складываются: две смерти на одной клетке — это одна куча падали,
// а не две отдельные.
//
// Вызывать только из команды очереди (05_Entity.md, п.5): добавление
// компонента — структурное изменение.
void depositCarcass(World& world, int x, int y, float meat, int protein) {
    if ((meat <= 0.0f && protein <= 0) || !world.area().inBounds(x, y)) {
        return;
    }
    for (const auto tile : world.area().cellAt(x, y).entities) {
        if (!world.registry().all_of<SoilComponent>(tile)) {
            continue;
        }
        if (auto* carcass = world.registry().try_get<CarcassComponent>(tile)) {
            carcass->meat += meat;
            carcass->protein += protein;
        } else {
            world.registry().emplace<CarcassComponent>(tile, CarcassComponent{meat, protein, 0.0f});
        }
        return;
    }
}

// Крупицы белка распределены по туше равномерно, поэтому убыль мяса —
// съеденного или сгнившего — освобождает их пропорционально. Куда они
// пойдут дальше (в едока или в перегной), решает вызывающая сторона: для
// самой туши это одно и то же убывание.
int releaseCarcassProtein(CarcassComponent& carcass, float meatBefore, float removed) {
    if (meatBefore <= 0.0f || removed <= 0.0f || carcass.protein <= 0) {
        return 0;
    }
    carcass.proteinPending += static_cast<float>(carcass.protein) * std::min(1.0f, removed / meatBefore);
    int released = 0;
    while (carcass.proteinPending >= 1.0f && carcass.protein > 0) {
        carcass.proteinPending -= 1.0f;
        --carcass.protein;
        ++released;
    }
    return released;
}

// Смерть животного — один путь для всех причин: старость, истощение,
// чужие зубы. Тело ложится падалью на ту клетку, где животное легло, и
// дальше его либо съедят, либо оно сгниёт в перегной.
void enqueueDeath(CommandQueue& commands, entt::entity entity, int x, int y) {
    commands.enqueue([entity, x, y](World& w) {
        if (!w.registry().valid(entity)) {
            return;
        }
        // Читаем состояние заново, а не полагаемся на снятое при постановке
        // команды: пока она ждала очереди, тик мог доработать
        // (05_Entity.md, п.5).
        float meat = 0.0f;
        int protein = 0;
        if (const auto* body = w.registry().try_get<const AnimalComponent>(entity)) {
            const float size = kMinSizeShare + (1.0f - kMinSizeShare) * body->growth;
            meat = kMeatPerSize * size;
            // И накопленный белок, и не вышедший навоз: из тела в мир
            // уходит всё, что в нём было.
            protein = body->protein + body->dung;
        }
        depositCarcass(w, x, y, meat, protein);
        w.despawn(entity);
    });
}

} // namespace

void AnimalSystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    auto& registry = world.registry();
    const auto& worldProperties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    const float mutationRate = worldProperties.animalMutationRate;
    const auto animalSeed = static_cast<std::uint64_t>(worldProperties.animalRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& species = registry.get<const AnimalSpeciesComponent>(world.worldEntity());

    // --- 1. Снимок животных ---
    // Разреженно, а не плотным массивом на всю Область: животных десятки, а
    // клеток десятки тысяч (в отличие от почвы и травы, которых по одной на
    // клетку). Плотный массив здесь перебирал бы 10 000 клеток ради
    // тридцати существ.
    std::vector<Animal> animals;
    auto animalView =
        registry.view<AnimalComponent, AnimalGenomeComponent, DesireComponent, IdentityComponent,
                       PositionComponent>();
    for (const auto entity : animalView) {
        const auto& position = animalView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        animals.push_back(Animal{entity, animalView.get<IdentityComponent>(entity).id, position.x, position.y,
                                  registry.all_of<PredatorComponent>(entity),
                                  &animalView.get<AnimalComponent>(entity),
                                  &animalView.get<AnimalGenomeComponent>(entity),
                                  &animalView.get<DesireComponent>(entity)});
    }

    // Падаль живёт своей жизнью и без животных (гниёт), поэтому выйти
    // раньше времени нельзя — но если нет ни того, ни другого, делать
    // системе действительно нечего.
    const bool anyCarcass = !registry.view<CarcassComponent>().empty();
    if (animals.empty() && !anyCarcass) {
        return;
    }

    // --- 2. Снимок тайлов ---
    // А вот это — данные тайла, по одному значению на клетку, поэтому здесь
    // плотные массивы уместны (тот же приём, что в HydrologySystem и
    // PlantSystem). Снимок снимается ДО того, как хоть одно животное
    // тронулось с места: все решения этого тика принимаются по одному и
    // тому же состоянию мира, и порядок обхода на них не влияет.
    const entt::entity kNullEntity = entt::null;
    std::vector<entt::entity> terrain(cellCount, kNullEntity);
    std::vector<float> waterAt(cellCount, 0.0f);
    std::vector<entt::entity> plantAt(cellCount, kNullEntity);
    std::vector<float> plantGrowth(cellCount, 0.0f);
    std::vector<float> carcassMeat(cellCount, 0.0f);

    auto terrainView = registry.view<PositionComponent, SoilComponent>();
    for (const auto entity : terrainView) {
        const auto& position = terrainView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        terrain[i] = entity;
        if (const auto* water = registry.try_get<const WaterComponent>(entity)) {
            waterAt[i] = water->depth;
        }
        if (const auto* carcass = registry.try_get<const CarcassComponent>(entity)) {
            carcassMeat[i] = carcass->meat;
        }
    }

    auto plantView = registry.view<PlantComponent, PositionComponent>();
    for (const auto entity : plantView) {
        const auto& position = plantView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        const std::size_t i = index(position.x, position.y);
        plantAt[i] = entity;
        plantGrowth[i] = plantView.get<PlantComponent>(entity).growth;
    }

    std::vector<ShareIntent> bites;    // трава
    std::vector<ShareIntent> meals;    // падаль
    std::vector<ShareIntent> drinks;
    std::vector<StepIntent> steps;
    std::vector<MateIntent> matings;
    std::vector<AttackIntent> attacks;

    // Откуда исходит опасность — считается в проходе 3 и используется в
    // проходе 4. Живёт здесь, а не в компоненте: система не хранит
    // состояние между тиками (05_Entity.md, п.3), а животное не помнит
    // хищника, которого больше не видит.
    std::vector<int> threatX(animals.size(), 0);
    std::vector<int> threatY(animals.size(), 0);

    // --- 3. Тело и желания ---
    // Отдельным проходом от решений (п.4) намеренно: животное, выбирая
    // пару, смотрит, чего хочет сосед, — и если бы желания и решения
    // считались в одном проходе, сосед, которого EnTT хранит позже, был бы
    // ещё с прошлотиковым желанием. Порядок в памяти не может быть причиной
    // события в мире (02_CorePrinciples.md, п.12a), поэтому сначала весь
    // мир узнаёт, чего он хочет, и только потом кто-то что-то делает.
    std::vector<bool> alive(animals.size(), true);
    for (std::size_t a = 0; a < animals.size(); ++a) {
        const Animal& animal = animals[a];
        auto& state = *animal.state;
        const auto& genome = *animal.genome;
        auto& desire = *animal.desire;

        const float size = kMinSizeShare + (1.0f - kMinSizeShare) * state.growth;

        // Цена существования. Тратится всегда — стоящее на месте животное
        // тоже живёт.
        state.energy -= genome.energyUpkeep * size;
        state.water -= genome.waterUpkeep * size;
        state.age += 1.0f;

        // Пищеварение: белок не лежит в теле вечно, часть уходит навозом.
        // Отсюда и постоянная нужда есть, а не только "когда кончилась
        // энергия".
        state.dungPending += kDungRate * size;
        while (state.dungPending >= 1.0f && state.protein > 0) {
            state.dungPending -= 1.0f;
            --state.protein;
            ++state.dung;
        }
        state.dungPending = std::min(state.dungPending, 1.0f);

        // Взросление: детёныш дорастает до взрослого примерно к
        // maturityAge, но не выше того, что позволяет накопленный белок —
        // ровно как размер растения ограничен его минералами. Голодное
        // животное не растёт вовсе.
        if (state.growth < 1.0f && state.energy > 0.0f) {
            const float proteinCeiling =
                genome.proteinNeed > 0.0f ? static_cast<float>(state.protein) / genome.proteinNeed : 1.0f;
            const float ceiling = std::min(1.0f, std::max(state.growth, proteinCeiling));
            const float rate = genome.maturityAge > 1.0f ? 1.0f / genome.maturityAge : 1.0f;
            state.growth = std::clamp(state.growth + rate, 0.0f, ceiling);
        }

        if (state.energy <= 0.0f) {
            state.energy = 0.0f;
            state.stress += kStarvationStress;
        }
        if (state.water <= 0.0f) {
            state.water = 0.0f;
            state.stress += kDehydrationStress;
        }
        if (state.energy > 0.0f && state.water > 0.0f) {
            state.stress = std::max(0.0f, state.stress - kStressRelief);
            // Раны затягиваются только у того, кому есть чем.
            state.health = std::min(1.0f, state.health + kHealRate);
        }

        // Смерть от старости, от условий или от чужих зубов. Entity
        // исчезает не сейчас, а при разрешении очереди команд
        // (05_Entity.md, п.5), и тело ложится падалью — одинаково, от чего
        // бы животное ни умерло.
        if (state.age >= genome.maxAge || state.stress >= 1.0f || state.health <= 0.0f) {
            enqueueDeath(commands, animal.entity, animal.x, animal.y);
            alive[a] = false;
            continue;
        }

        // Навоз выпадает порциями на ту клетку, где животное сейчас стоит.
        if (state.dung >= kDungDrop) {
            const int dropped = state.dung;
            state.dung = 0;
            commands.enqueue([x = animal.x, y = animal.y, dropped](World& w) { depositHumus(w, x, y, dropped); });
        }

        // --- Желания ---
        // Голод и жажда не копятся сами по себе — они читаются из тела, и
        // потому падают сразу, как только животное поело или напилось. Это
        // и есть удовлетворение желания: изменившееся тело, а не списанный
        // счётчик.
        const float energyDeficit =
            genome.energyCapacity > 0.0f ? 1.0f - state.energy / genome.energyCapacity : 1.0f;
        const float proteinDeficit =
            genome.proteinNeed > 0.0f ? 1.0f - static_cast<float>(state.protein) / genome.proteinNeed : 0.0f;
        desire.hunger = std::clamp(std::max(energyDeficit, proteinDeficit), 0.0f, 1.0f);
        desire.thirst =
            std::clamp(genome.waterCapacity > 0.0f ? 1.0f - state.water / genome.waterCapacity : 1.0f, 0.0f, 1.0f);

        // Страх: насколько близко видна опасность. Хищник ни от кого не
        // бегает — на него в этом мире не охотятся (09_Animals.md, п.2).
        desire.fear = 0.0f;
        if (!animal.predator) {
            const float sight = std::max(1.0f, genome.perception);
            for (std::size_t b = 0; b < animals.size(); ++b) {
                if (!animals[b].predator) {
                    continue;
                }
                const int dx = animals[b].x - animal.x;
                const int dy = animals[b].y - animal.y;
                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (distance > sight) {
                    continue;
                }
                // Увидел — беги. Страх растёт по мере приближения зубов, но
                // и на самом краю видимости он уже выше порога желаний
                // (kDesireFloor): замеченный хищник — всегда повод уходить,
                // а не только тот, что стоит рядом.
                //
                // Разница огромна: пока травоядное срывалось с места лишь
                // тогда, когда хищник подходил вплотную, оно отдавало ему
                // половину форы, погоня выходила короткой и кончалась
                // одинаково — стадо выбивалось под ноль за несколько тысяч
                // тиков, а следом вымирали и сами хищники. Фора, которую
                // даёт зоркость, и есть главная защита добычи.
                const float scare = kDesireFloor + (1.0f - kDesireFloor) * (1.0f - distance / sight);
                if (scare > desire.fear) {
                    desire.fear = scare;
                    threatX[a] = animals[b].x;
                    threatY[a] = animals[b].y;
                }
            }
        }

        const bool adult = state.age >= genome.maturityAge && state.growth >= kBreedingGrowth;
        // Не бедствует: ни голод, ни жажда не дошли до предела, и нет
        // накопленного стресса — то есть в последние десятки тиков запасы
        // не кончались совсем.
        const bool content =
            state.stress <= 0.0f && desire.hunger < kCalmNeed && desire.thirst < kCalmNeed;
        if (adult && content) {
            // Желание пары — единственное, которого в теле не прочитать: оно
            // копится со временем у того, кому больше нечего хотеть.
            desire.mating = std::min(1.0f, desire.mating + genome.breedingUrge);
        }
        desire.current = chooseDesire(desire, adult && content);
    }

    // --- 4. Решения: что животное делает со своим желанием ---
    for (std::size_t a = 0; a < animals.size(); ++a) {
        if (!alive[a]) {
            continue;
        }
        const Animal& animal = animals[a];
        auto& state = *animal.state;
        const auto& genome = *animal.genome;
        auto& desire = *animal.desire;
        const std::size_t here = index(animal.x, animal.y);
        const float size = kMinSizeShare + (1.0f - kMinSizeShare) * state.growth;

        // Случайность собирается из seed мира, номера тика и постоянного
        // идентификатора животного (core/Random.hpp). Не из координат, как
        // у растения: клетка у животного меняется каждый шаг, а на одной
        // клетке их может стоять несколько — розыгрыш вышел бы у них
        // одинаковым (см. IdentityComponent).
        std::uint64_t random = mixSeed(animalSeed, mixSeed(tick, animal.id));

        // --- Действие ---
        // Либо желаемое рядом — и животное занимается им, никуда не идя,
        // либо оно видит, куда идти, и делает шаг. Дальше видимости
        // (perception) для животного ничего не существует: оно не может
        // хотеть того, о чём не знает (02_CorePrinciples.md, п.6).
        const int reach = std::max(1, static_cast<int>(std::lround(genome.perception)));
        bool busy = false;
        bool hasTarget = false;
        int targetX = animal.x;
        int targetY = animal.y;

        // Куда животное вообще может встать: не за границей Области, не на
        // занятый непроходимым объектом тайл и не в воду. Вода — стена:
        // животное не плавает и брода не знает, поэтому клетка, где вода
        // есть вообще (waterAt > 0, то есть на тайле висит WaterComponent),
        // непроходима для него так же, как булыжник. Река делит карту, а не
        // замедляет ход.
        //
        // Этим же проверяется и годность еды: до туши, лежащей посреди
        // разлившейся реки, не дойти — значит, для животного это не еда, а
        // вид. Без этой проверки хищник вставал у самой воды напротив
        // недосягаемой туши и голодал насмерть, каждый тик заново выбирая
        // её целью: в мире была еда, до которой он не мог дойти, и он не
        // умел этого понять.
        //
        // Тот же закон действует и при расселении первого поголовья
        // (AnimalSeeding.cpp): в воду не ставим — животное туда и само не
        // пойдёт. Разойдись эти две проверки, и стадо оказалось бы
        // расставленным там, откуда оно не может сойти.
        auto standable = [&](std::size_t cell, int nx, int ny) {
            return !world.area().isBlocked(nx, ny) && terrain[cell] != entt::null && waterAt[cell] <= 0.0f;
        };

        // Ближайшая клетка в пределах видимости, удовлетворяющая условию.
        // Ближайшая, а не лучшая: животное идёт к тому, что видит рядом, а
        // не выбирает оптимум по всей округе.
        //
        // Из одинаково близких выбор бросается жребием, а не достаётся
        // первой по обходу. Обход идёт с левого верхнего угла квадрата
        // видимости, и без жребия стадо на ровном лугу, где еда со всех
        // сторон одинаково близко, дружно уходило бы вверх и влево — не
        // потому, что там лучше, а потому, что цикл начинается оттуда.
        auto findNearest = [&](auto predicate, int& outX, int& outY) {
            float bestDistance = 0.0f;
            int ties = 0;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int nx = animal.x + dx;
                    const int ny = animal.y + dy;
                    if (!world.area().inBounds(nx, ny)) {
                        continue;
                    }
                    const float distance = static_cast<float>(dx * dx + dy * dy);
                    if (distance > static_cast<float>(reach * reach)) {
                        continue; // видимость круглая, а не квадратная
                    }
                    if (ties > 0 && distance > bestDistance) {
                        continue;
                    }
                    if (!predicate(index(nx, ny), nx, ny)) {
                        continue;
                    }
                    if (ties > 0 && distance == bestDistance) {
                        // Равноудалённая находка: занимает место прежней с
                        // вероятностью 1/N, поэтому все они равноправны.
                        ++ties;
                        if (randomUnit(random) >= 1.0f / static_cast<float>(ties)) {
                            continue;
                        }
                    } else {
                        ties = 1;
                    }
                    bestDistance = distance;
                    outX = nx;
                    outY = ny;
                }
            }
            return ties > 0 ? bestDistance : -1.0f;
        };

        switch (desire.current) {
            case Desire::Food: {
                if (animal.predator) {
                    // Хищник ест не жертву, а падаль: сначала надо убить
                    // (или найти уже мёртвое), и только потом есть.
                    if (carcassMeat[here] > kMinBiteMeat) {
                        meals.push_back(ShareIntent{here, static_cast<int>(a), animal.id, genome.biteSize * size});
                        busy = true;
                        break;
                    }

                    // Добыча в пределах досягаемости зубов — бьём. Ближе
                    // всех и первой: гнаться за той, что дальше, когда рядом
                    // стоит эта, бессмысленно. Но только если хищник и
                    // вправду голоден (kHuntHunger): наевшийся не охотится.
                    int preyIndex = -1;
                    float preyDistance = 0.0f;
                    for (std::size_t b = 0; desire.hunger >= kHuntHunger && b < animals.size(); ++b) {
                        if (b == a || !alive[b] || animals[b].predator) {
                            continue;
                        }
                        // За тем, кто быстрее, гнаться незачем: догнать его
                        // нельзя, а силы уйдут. Скорость чужого бега —
                        // ровно то знание, которое у хищника есть: он её
                        // видит (02_CorePrinciples.md, п.6), в отличие от
                        // чужого возраста или запаса сил.
                        //
                        // Без этого правила хищник выбирал ближайшую добычу
                        // и гнался за ней, даже если она заведомо уходила;
                        // на безнадёжные погони уходила вся энергия, и
                        // первое поголовье вымирало за тысячу тиков в мире,
                        // полном добычи. Заодно у стада появляется смысл
                        // вкладывать бюджет в скорость: быстрого не
                        // преследуют вовсе.
                        if (animals[b].genome->speed >= genome.speed) {
                            continue;
                        }
                        const int dx = animals[b].x - animal.x;
                        const int dy = animals[b].y - animal.y;
                        const float distance = static_cast<float>(dx * dx + dy * dy);
                        if (distance > static_cast<float>(reach * reach)) {
                            continue;
                        }
                        if (preyIndex >= 0 && distance >= preyDistance) {
                            continue;
                        }
                        preyIndex = static_cast<int>(b);
                        preyDistance = distance;
                    }

                    if (preyIndex >= 0 &&
                        std::abs(animals[static_cast<std::size_t>(preyIndex)].x - animal.x) <= kAttackReach &&
                        std::abs(animals[static_cast<std::size_t>(preyIndex)].y - animal.y) <= kAttackReach) {
                        attacks.push_back(AttackIntent{preyIndex, genome.attack * size});
                        busy = true;
                        break;
                    }

                    // Видимая падаль важнее видимой добычи: зачем гнаться,
                    // когда рядом лежит мясо. Хищнику всё равно, кто убил
                    // ту тушу, — падаль это тоже еда (см.
                    // PredatorComponent).
                    //
                    // Правило не косметическое. Пока хищник шёл к тому, что
                    // ближе, он бросал недоеденную тушу ради свежей добычи
                    // и убивал куда больше, чем съедал: по карте лежали
                    // десятки почти нетронутых туш, а стадо тем временем
                    // выбивалось под ноль — и следом вымирали сами хищники.
                    // "Сначала доешь" превращает лишнее убийство в редкость,
                    // а не в правило.
                    int carcassX = animal.x;
                    int carcassY = animal.y;
                    const float carcassDistance = findNearest(
                        [&](std::size_t cell, int nx, int ny) {
                            return carcassMeat[cell] > kMinBiteMeat && standable(cell, nx, ny);
                        },
                        carcassX, carcassY);
                    if (carcassDistance >= 0.0f) {
                        targetX = carcassX;
                        targetY = carcassY;
                        hasTarget = true;
                    } else if (preyIndex >= 0) {
                        targetX = animals[static_cast<std::size_t>(preyIndex)].x;
                        targetY = animals[static_cast<std::size_t>(preyIndex)].y;
                        hasTarget = true;
                    }
                    break;
                }

                if (plantAt[here] != entt::null && plantGrowth[here] > kMinBiteGrowth) {
                    bites.push_back(ShareIntent{here, static_cast<int>(a), animal.id, genome.biteSize * size});
                    busy = true;
                } else {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return plantAt[cell] != entt::null && plantGrowth[cell] > kMinBiteGrowth &&
                                               standable(cell, nx, ny);
                                    },
                                    targetX, targetY) >= 0.0f;
                }
                break;
            }
            case Desire::Water: {
                // Пьёт со своей клетки или с любой соседней: животное
                // стоит на берегу, а не заходит в реку — шагнуть в воду оно
                // и не может (см. standable). Своя клетка в проверке всё
                // равно нужна: паводок может залить ту, на которой животное
                // стоит.
                std::size_t source = cellCount;
                if (waterAt[here] > 0.0f) {
                    source = here;
                } else {
                    for (int dir = 0; dir < 8; ++dir) {
                        const int nx = animal.x + kDx8[dir];
                        const int ny = animal.y + kDy8[dir];
                        if (!world.area().inBounds(nx, ny)) {
                            continue;
                        }
                        const std::size_t j = index(nx, ny);
                        if (waterAt[j] > 0.0f) {
                            source = j;
                            break;
                        }
                    }
                }
                if (source < cellCount) {
                    drinks.push_back(ShareIntent{source, static_cast<int>(a), animal.id, kDrinkRate * size});
                    busy = true;
                } else {
                    hasTarget =
                        findNearest([&](std::size_t cell, int, int) { return waterAt[cell] > 0.0f; }, targetX,
                                     targetY) >= 0.0f;
                }
                break;
            }
            case Desire::Mate: {
                // Партнёров ищем перебором по всем животным, а не по клеткам
                // карты: их десятки, и квадрат от десятков дешевле, чем
                // просмотр круга клеток на каждого.
                float bestDistance = 0.0f;
                for (std::size_t b = 0; b < animals.size(); ++b) {
                    if (b == a || !alive[b]) {
                        continue;
                    }
                    const Animal& other = animals[b];
                    if (other.predator != animal.predator || other.genome->species != genome.species ||
                        other.state->sex == state.sex) {
                        continue; // пара — своего вида и своей диеты
                    }
                    if (other.desire->current != Desire::Mate) {
                        continue; // пара нужна согласная: тот, кто занят едой, не сойдётся
                    }
                    const int dx = other.x - animal.x;
                    const int dy = other.y - animal.y;
                    const float distance = static_cast<float>(dx * dx + dy * dy);
                    if (distance > static_cast<float>(reach * reach)) {
                        continue;
                    }
                    if (hasTarget && distance >= bestDistance) {
                        continue;
                    }
                    bestDistance = distance;
                    targetX = other.x;
                    targetY = other.y;
                    hasTarget = true;
                }

                // Навстречу идёт только самец, самка при виде него ждёт на
                // месте. Это не украшение поведения, а необходимость: два
                // животных, шагающих друг к другу, каждый тик меняются
                // клетками и остаются соседями — они могут так и не
                // встретиться, продолжая всю жизнь ходить друг сквозь
                // друга. Кто именно ждёт, мир решает полом, а не жребием:
                // жребий пришлось бы бросать заново каждый тик, и пара
                // снова начала бы топтаться.
                if (hasTarget && (targetX != animal.x || targetY != animal.y) && state.sex == Sex::Female) {
                    hasTarget = false;
                    busy = true;
                }

                if (hasTarget && targetX == animal.x && targetY == animal.y) {
                    matings.push_back(MateIntent{here, static_cast<int>(a), animal.id, genome.species,
                                                  animal.predator, state.sex});
                    hasTarget = false;
                    busy = true;
                }
                break;
            }
            case Desire::Flee:
                // От опасности не идут "к цели" — от неё уходят, поэтому
                // направление шага считается ниже отдельно, а цели у
                // бегущего нет.
                break;
            case Desire::Idle:
                break;
        }

        // --- Шаг ---
        // Занятое животное (ест, пьёт, бьёт, сошлось с парой) с места не
        // сходит.
        if (busy) {
            continue;
        }

        state.stepProgress += genome.speed;
        if (state.stepProgress < 1.0f) {
            continue;
        }
        state.stepProgress -= 1.0f;

        const bool fleeing = desire.current == Desire::Flee;

        // Куда идти, когда желаемого не видно (или когда до него нет
        // дороги, см. ниже): направление отрезка поиска. Берётся из
        // постоянного идентификатора и номера отрезка, поэтому пересчёт
        // даёт то же самое, сколько раз за тик его ни спроси.
        auto roamTarget = [&](int& rx, int& ry) {
            std::uint64_t roam = mixSeed(animal.id, tick / kRoamTicks);
            const int dir = static_cast<int>(nextState(roam) % 8u);
            rx = animal.x + kDx8[dir] * kRoamReach;
            ry = animal.y + kDy8[dir] * kRoamReach;
        };

        // Желаемого не видно — значит, надо искать. Ищущее животное идёт в
        // одну сторону целый отрезок пути (kRoamTicks), а не топчется на
        // месте: за пределами собственной видимости другого способа
        // что-нибудь найти у него нет. Ищут все трое — и еду, и воду, и
        // пару; река, до которой не дошли, убивает вернее любых зубов.
        bool roaming = false;
        if (!fleeing && !hasTarget &&
            (desire.current == Desire::Food || desire.current == Desire::Water ||
             desire.current == Desire::Mate)) {
            roamTarget(targetX, targetY);
            hasTarget = true;
            roaming = true;
        }

        if (!fleeing && !hasTarget && randomUnit(random) >= kWanderChance) {
            continue; // ничего не гонит — стоит и щиплет что придётся
        }

        // Куда именно: из проходимых соседей выбирается тот, что ближе к
        // цели (а бегущим — тот, что дальше от опасности); без цели —
        // случайный. На занятый непроходимым объектом тайл и в воду
        // животное не идёт, а вот на клетку с травой и с другим животным —
        // сколько угодно.
        int stepX = animal.x;
        int stepY = animal.y;
        bool stepFound = false;
        const int firstDir = static_cast<int>(randomUnit(random) * 8.0f) % 8;

        // Один и тот же перебор соседей для любой цели: идущий выбирает
        // ближайшего к ней, бегущий — самого дальнего от опасности,
        // разница только в знаке сравнения. Возвращает, приближает ли
        // найденный шаг к цели: этим отличается дорога от тупика.
        auto stepToward = [&](int aimX, int aimY, bool aimless) {
            auto scoreOf = [&](int x, int y) {
                const float dx = static_cast<float>(aimX - x);
                const float dy = static_cast<float>(aimY - y);
                return dx * dx + dy * dy;
            };

            stepFound = false;
            float bestScore = 0.0f;
            for (int n = 0; n < 8; ++n) {
                const int dir = (firstDir + n) % 8;
                const int nx = animal.x + kDx8[dir];
                const int ny = animal.y + kDy8[dir];
                if (!world.area().inBounds(nx, ny) || !standable(index(nx, ny), nx, ny)) {
                    continue;
                }
                if (aimless) {
                    stepX = nx;
                    stepY = ny;
                    stepFound = true;
                    break; // случайное блуждание: первый же годный сосед по кругу
                }
                const float distance = scoreOf(nx, ny);
                const bool better = fleeing ? distance > bestScore : distance < bestScore;
                if (!stepFound || better) {
                    bestScore = distance;
                    stepX = nx;
                    stepY = ny;
                    stepFound = true;
                }
            }
            return stepFound && (aimless || (fleeing ? bestScore > scoreOf(animal.x, animal.y)
                                                     : bestScore < scoreOf(animal.x, animal.y)));
        };

        const bool aimless = !fleeing && !hasTarget;
        const bool approached = stepToward(fleeing ? threatX[a] : targetX, fleeing ? threatY[a] : targetY, aimless);

        // Дороги нет: ни один сосед не подводит к цели ближе — между
        // животным и желаемым вода или камень. Стоять и смотреть на
        // недостижимое нельзя: голод сам не пройдёт, а цель будет
        // выбираться заново каждый тик, одна и та же, — зверь так и умрёт
        // на берегу напротив луга. Поэтому упёршийся идёт вдоль преграды —
        // тем же отрезком поиска, каким ищут невидимое: обойти реку или
        // не обойти, он не знает, но стоя не узнает точно.
        //
        // Раньше на этом месте животное лезло в воду вброд. Теперь вода
        // непроходима совсем, и обход — единственное, что у него осталось:
        // паводок (HydrologySystem) режет карту на острова, и застрявшему
        // на острове зверю остаётся только доесть его и умереть.
        if (!approached && !aimless && !fleeing && !roaming) {
            int detourX = animal.x;
            int detourY = animal.y;
            roamTarget(detourX, detourY);
            stepToward(detourX, detourY, false);
        }
        if (!stepFound) {
            continue;
        }

        state.energy = std::max(0.0f, state.energy - kStepEnergy * size);
        steps.push_back(StepIntent{static_cast<int>(a), stepX, stepY});
    }

    // --- 5. Кормёжка травоядных: один куст на всех, кто до него дотянулся ---
    // Спор делится долями: если куста на всех не хватает, каждый получает
    // свою часть. Порядок внутри клетки — по постоянному идентификатору, а
    // не по порядку в хранилище: крупицы белка целые, и кому достанется
    // последняя, решает мир, а не EnTT.
    std::sort(bites.begin(), bites.end(), sortByCellThenId);
    for (std::size_t n = 0; n < bites.size();) {
        std::size_t m = n;
        float demand = 0.0f;
        while (m < bites.size() && bites[m].cell == bites[n].cell) {
            demand += bites[m].want;
            ++m;
        }

        const entt::entity plantEntity = plantAt[bites[n].cell];
        auto* plant = registry.valid(plantEntity) ? registry.try_get<PlantComponent>(plantEntity) : nullptr;
        if (plant == nullptr || demand <= 0.0f) {
            n = m;
            continue;
        }

        const float growthBefore = plant->growth;
        const float moistureBefore = plant->moisture;
        const int mineralsBefore = plant->minerals;
        const float share = std::min(1.0f, growthBefore / demand);

        float eatenTotal = 0.0f;
        for (std::size_t k = n; k < m; ++k) {
            const float eaten = bites[k].want * share;
            if (eaten <= 0.0f) {
                continue;
            }
            auto& state = *animals[static_cast<std::size_t>(bites[k].animal)].state;
            const auto& genome = *animals[static_cast<std::size_t>(bites[k].animal)].genome;

            state.energy = std::min(genome.energyCapacity, state.energy + eaten * kEnergyPerBiomass * genome.digestion);

            // Доля куста, доставшаяся этому животному, — по ней делятся и
            // вода, и крупицы белка: съеденное растение отдаёт всё, что в
            // нём было, а не только биомассу.
            const float fraction = growthBefore > 0.0f ? eaten / growthBefore : 0.0f;
            const float sap = moistureBefore * fraction;
            state.water = std::min(genome.waterCapacity, state.water + sap * kWaterPerBiomass);
            plant->moisture = std::max(0.0f, plant->moisture - sap);

            const int proteinCap = std::max(1, static_cast<int>(std::ceil(genome.proteinNeed)));
            state.proteinPending += static_cast<float>(mineralsBefore) * fraction;
            while (state.proteinPending >= 1.0f && plant->minerals > 0) {
                state.proteinPending -= 1.0f;
                --plant->minerals;
                if (state.protein < proteinCap) {
                    ++state.protein;
                } else {
                    // Тело больше не удержит — крупица проходит насквозь и
                    // ляжет навозом там, где животное окажется.
                    ++state.dung;
                }
            }
            state.proteinPending = std::min(state.proteinPending, 1.0f);

            eatenTotal += eaten;
        }

        plant->growth = std::max(0.0f, plant->growth - eatenTotal);
        // Само по себе объедание растение не убивает: оно теряет биомассу и
        // получает стресс, а погибнуть или отрасти — решит PlantSystem по
        // своим законам на следующем тике.
        plant->stress = std::min(1.0f, plant->stress + eatenTotal * kGrazeStress);
        n = m;
    }

    // --- 6. Кормёжка хищников: одна туша на всех, кто до неё добрался ---
    // Тот же закон дележа, что и у травы: спор решается долями, а целые
    // крупицы белка расходятся по порядку идентификаторов.
    std::sort(meals.begin(), meals.end(), sortByCellThenId);
    for (std::size_t n = 0; n < meals.size();) {
        std::size_t m = n;
        float demand = 0.0f;
        while (m < meals.size() && meals[m].cell == meals[n].cell) {
            demand += meals[m].want;
            ++m;
        }

        const entt::entity tile = terrain[meals[n].cell];
        auto* carcass =
            tile != entt::null && registry.valid(tile) ? registry.try_get<CarcassComponent>(tile) : nullptr;
        if (carcass == nullptr || demand <= 0.0f) {
            n = m;
            continue;
        }

        const float meatBefore = carcass->meat;
        const float share = std::min(1.0f, meatBefore / demand);

        for (std::size_t k = n; k < m; ++k) {
            const float eaten = meals[k].want * share;
            if (eaten <= 0.0f) {
                continue;
            }
            auto& state = *animals[static_cast<std::size_t>(meals[k].animal)].state;
            const auto& genome = *animals[static_cast<std::size_t>(meals[k].animal)].genome;

            state.energy = std::min(genome.energyCapacity, state.energy + eaten * kEnergyPerBiomass * genome.digestion);
            // В мясе есть кровь: наевшийся хищник какое-то время может не
            // искать воду.
            state.water = std::min(genome.waterCapacity, state.water + eaten * kWaterPerMeat);

            const float meatNow = carcass->meat;
            carcass->meat = std::max(0.0f, carcass->meat - eaten);
            const int grains = releaseCarcassProtein(*carcass, meatNow, meatNow - carcass->meat);

            const int proteinCap = std::max(1, static_cast<int>(std::ceil(genome.proteinNeed)));
            for (int g = 0; g < grains; ++g) {
                if (state.protein < proteinCap) {
                    ++state.protein;
                } else {
                    ++state.dung;
                }
            }
        }
        n = m;
    }

    // --- 7. Водопой: так же долями ---
    std::sort(drinks.begin(), drinks.end(), sortByCellThenId);
    for (std::size_t n = 0; n < drinks.size();) {
        std::size_t m = n;
        float demand = 0.0f;
        while (m < drinks.size() && drinks[m].cell == drinks[n].cell) {
            demand += drinks[m].want;
            ++m;
        }

        const std::size_t cell = drinks[n].cell;
        const entt::entity tile = terrain[cell];
        auto* water = tile != entt::null && registry.valid(tile) ? registry.try_get<WaterComponent>(tile) : nullptr;
        if (water == nullptr || demand <= 0.0f) {
            n = m;
            continue;
        }

        const float availableUnits = water->depth / kDrinkDepthPerUnit;
        const float share = std::min(1.0f, availableUnits / demand);
        float drunk = 0.0f;
        for (std::size_t k = n; k < m; ++k) {
            auto& state = *animals[static_cast<std::size_t>(drinks[k].animal)].state;
            const auto& genome = *animals[static_cast<std::size_t>(drinks[k].animal)].genome;
            const float got = std::min(drinks[k].want * share, std::max(0.0f, genome.waterCapacity - state.water));
            state.water += got;
            drunk += got;
        }
        // Выпитое уходит из клетки: воды в мире от водопоя становится
        // меньше, пусть и на малость. Исчезнет ли после этого сама вода с
        // тайла — решает HydrologySystem по своему порогу, не мы.
        water->depth = std::max(0.0f, water->depth - drunk * kDrinkDepthPerUnit);
        n = m;
    }

    // --- 8. Зубы ---
    // Урон складывается: двое хищников на одной жертве валят её вдвое
    // быстрее. Смерть от ран разрешается здесь же, а не на следующем тике,
    // — иначе убитое животное успело бы ещё раз пошевелиться. Каждая
    // жертва хоронится один раз, сколько бы зубов в ней ни сомкнулось.
    for (const auto& attack : attacks) {
        const auto prey = static_cast<std::size_t>(attack.prey);
        if (!alive[prey]) {
            continue;
        }
        animals[prey].state->health -= attack.damage;
    }
    for (std::size_t a = 0; a < animals.size(); ++a) {
        if (!alive[a] || animals[a].state->health > 0.0f) {
            continue;
        }
        enqueueDeath(commands, animals[a].entity, animals[a].x, animals[a].y);
        alive[a] = false;
    }

    // --- 9. Шаги ---
    // Применяются после всех решений: пока животные решают, мир для них
    // неподвижен, иначе сдвинувшийся сосед менял бы решение тех, кого
    // EnTT хранит позже. Убитые в п.8 никуда уже не идут.
    for (const auto& step : steps) {
        const auto a = static_cast<std::size_t>(step.animal);
        if (!alive[a]) {
            continue;
        }
        world.moveTo(animals[a].entity, step.x, step.y);

        // Вытаптывание: под ногами почва становится плотнее. Это и есть
        // обратная связь стада на луг помимо поедания — по тропам к
        // водопою трава растёт хуже (habitatFit по compaction).
        const entt::entity tile = terrain[index(step.x, step.y)];
        if (tile != entt::null && registry.valid(tile)) {
            auto& soil = registry.get<SoilComponent>(tile);
            const float size = kMinSizeShare + (1.0f - kMinSizeShare) * animals[a].state->growth;
            soil.compaction = std::clamp(soil.compaction + kTrampleRate * size, 0.0f, 1.0f);
        }
    }

    // --- 10. Встречи: кто с кем сошёлся ---
    // Пары складываются внутри клетки, в порядке постоянных
    // идентификаторов: если на одной клетке сошлись трое, кому достанется
    // пара — вопрос их собственных имён в мире, а не порядка в хранилище.
    std::sort(matings.begin(), matings.end(), [](const MateIntent& a, const MateIntent& b) {
        if (a.cell != b.cell) {
            return a.cell < b.cell;
        }
        return a.id < b.id;
    });
    std::vector<bool> paired(matings.size(), false);
    for (std::size_t n = 0; n < matings.size(); ++n) {
        if (paired[n] || !alive[static_cast<std::size_t>(matings[n].animal)]) {
            continue;
        }
        std::size_t partner = matings.size();
        for (std::size_t k = n + 1; k < matings.size() && matings[k].cell == matings[n].cell; ++k) {
            if (paired[k] || !alive[static_cast<std::size_t>(matings[k].animal)] ||
                matings[k].predator != matings[n].predator || matings[k].species != matings[n].species ||
                matings[k].sex == matings[n].sex) {
                continue;
            }
            partner = k;
            break;
        }
        if (partner == matings.size()) {
            continue;
        }
        paired[n] = true;
        paired[partner] = true;

        const bool firstIsMother = matings[n].sex == Sex::Female;
        const Animal& mother =
            animals[static_cast<std::size_t>(firstIsMother ? matings[n].animal : matings[partner].animal)];
        const Animal& father =
            animals[static_cast<std::size_t>(firstIsMother ? matings[partner].animal : matings[n].animal)];

        const auto& motherGenome = *mother.genome;
        const auto& fatherGenome = *father.genome;
        std::uint64_t random = mixSeed(animalSeed, mixSeed(tick, mixSeed(mother.id, father.id)));

        // Таблица черт и список видов — по диете родителей: они у пары
        // всегда одни и те же (см. отбор партнёра выше).
        const auto traits = mother.predator ? predatorTraits() : herbivoreTraits();
        const auto& archetypes = mother.predator ? species.predators : species.herbivores;
        const auto& archetype =
            (motherGenome.species >= 0 && static_cast<std::size_t>(motherGenome.species) < archetypes.size())
                ? archetypes[static_cast<std::size_t>(motherGenome.species)]
                : motherGenome;
        const AnimalGenomeComponent childGenome =
            crossGenomes(traits, motherGenome, fatherGenome, archetype, mutationRate, random);

        AnimalComponent calf;
        calf.growth = kNewbornGrowth;
        calf.sex = randomUnit(random) < 0.5f ? Sex::Female : Sex::Male;

        // Детёныш появляется не из ниоткуда: и запасы, и крупица белка —
        // материнские. Ровно тот же обмен, что у растения с семенем.
        const float givenEnergy = mother.state->energy * kBirthEnergyShare;
        mother.state->energy -= givenEnergy;
        calf.energy = std::min(givenEnergy, childGenome.energyCapacity);

        const float givenWater = mother.state->water * kBirthWaterShare;
        mother.state->water -= givenWater;
        calf.water = std::min(givenWater, childGenome.waterCapacity);

        // Белок детёнышу — доля материнского, а не одна крупица. Крупица
        // — это ровно то, что нужно семечку травы, но не телу зверя:
        // детёныш, которому нужно девять крупиц на взрослый размер, с
        // одной остаётся мелким надолго, а мелкий хищник не может ни
        // охотиться (сила удара считается от размера), ни принести
        // потомство. Мать вкладывает в него треть накопленного — столько
        // же, сколько отдаёт запасов.
        const int givenProtein = std::max(0, mother.state->protein / 3);
        mother.state->protein -= givenProtein;
        calf.protein = givenProtein;
        calf.growth = childGenome.proteinNeed > 0.0f
                           ? std::min(calf.growth, static_cast<float>(calf.protein) / childGenome.proteinNeed)
                           : calf.growth;

        // Ухаживание тоже стоит энергии, но она никуда не переходит — это
        // потраченные силы, а не переданное вещество.
        father.state->energy =
            std::max(0.0f, father.state->energy - fatherGenome.energyCapacity * kCourtshipEnergyShare);

        mother.desire->mating = 0.0f;
        father.desire->mating = 0.0f;
        mother.desire->current = Desire::Idle;
        father.desire->current = Desire::Idle;

        const std::uint64_t calfId = mixSeed(random, mixSeed(mother.id, tick));
        const bool predatorChild = mother.predator;
        commands.enqueue([calf, childGenome, calfId, predatorChild, x = mother.x, y = mother.y](World& w) {
            const auto entity = w.registry().create();
            w.registry().emplace<IdentityComponent>(entity, IdentityComponent{calfId});
            w.registry().emplace<AnimalComponent>(entity, calf);
            w.registry().emplace<AnimalGenomeComponent>(entity, childGenome);
            // Новорождённый ничего ещё не хочет — тело у него полное; чего
            // хотеть, ему скажет первый же тик (см. п.3 выше).
            w.registry().emplace<DesireComponent>(entity, DesireComponent{});
            // Диета наследуется без вариантов: у травоядных родителей не
            // родится хищник.
            if (predatorChild) {
                w.registry().emplace<PredatorComponent>(entity);
            } else {
                w.registry().emplace<HerbivoreComponent>(entity);
            }
            // Проверять клетку не нужно: животное не занимает тайл
            // (04_WorldModel.md, п.4), поэтому детёныш всегда помещается
            // рядом с матерью.
            w.place(entity, x, y);
        });
    }

    // --- 11. Падаль гниёт ---
    // Несъеденная туша не лежит вечно. Мясо убывает, крупицы белка уходят
    // из неё тем же порядком, что и в желудок хищника, — только в перегной
    // (core/Humus.hpp), откуда их и вернёт в почву PlantSystem. Когда мяса
    // не остаётся, остаток белка уходит туда же, а компонент снимается
    // (02_CorePrinciples.md, п.3: нет падали — нет и компонента).
    auto carcassView = registry.view<CarcassComponent, PositionComponent>();
    for (const auto entity : carcassView) {
        auto& carcass = carcassView.get<CarcassComponent>(entity);
        const auto& position = carcassView.get<PositionComponent>(entity);

        const float meatBefore = carcass.meat;
        carcass.meat = std::max(0.0f, carcass.meat - kCarcassRot);
        int toHumus = releaseCarcassProtein(carcass, meatBefore, meatBefore - carcass.meat);

        if (carcass.meat <= 0.0f) {
            // Мяса не осталось — весь недоеденный белок уходит в землю
            // разом: держать его в пустой туше не за что.
            toHumus += carcass.protein;
            carcass.protein = 0;
        }

        if (toHumus > 0) {
            commands.enqueue([x = position.x, y = position.y, toHumus](World& w) { depositHumus(w, x, y, toHumus); });
        }

        if (carcass.meat <= 0.0f) {
            commands.enqueue([entity](World& w) {
                // Проверяем заново, а не полагаемся на снятое выше
                // состояние: пока команда ждала очереди, на этой же клетке
                // могло умереть ещё одно животное, и в туше снова есть мясо.
                auto* remains = w.registry().try_get<CarcassComponent>(entity);
                if (remains != nullptr && remains->meat <= 0.0f && remains->protein <= 0) {
                    w.registry().remove<CarcassComponent>(entity);
                }
            });
        }
    }
}


// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
void appendAnimalSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Animals (tick)";
    out.push_back({g, "kMinSizeShare", kMinSizeShare});
    out.push_back({g, "kEnergyPerBiomass", kEnergyPerBiomass});
    out.push_back({g, "kWaterPerBiomass", kWaterPerBiomass});
    out.push_back({g, "kWaterPerMeat", kWaterPerMeat});
    out.push_back({g, "kStepEnergy", kStepEnergy});
    out.push_back({g, "kDrinkRate", kDrinkRate});
    out.push_back({g, "kDrinkDepthPerUnit", kDrinkDepthPerUnit});
    out.push_back({g, "kMinBiteGrowth", kMinBiteGrowth});
    out.push_back({g, "kMinBiteMeat", kMinBiteMeat});
    out.push_back({g, "kGrazeStress", kGrazeStress});
    out.push_back({g, "kDungRate", kDungRate});
    out.push_back({g, "kDungDrop", static_cast<float>(kDungDrop)});
    out.push_back({g, "kStarvationStress", kStarvationStress});
    out.push_back({g, "kDehydrationStress", kDehydrationStress});
    out.push_back({g, "kStressRelief", kStressRelief});
    out.push_back({g, "kHealRate", kHealRate});
    out.push_back({g, "kDesireFloor", kDesireFloor});
    out.push_back({g, "kDesireSwitch", kDesireSwitch});
    out.push_back({g, "kBreedingGrowth", kBreedingGrowth});
    out.push_back({g, "kCalmNeed", kCalmNeed});
    out.push_back({g, "kMateDesire", kMateDesire});
    out.push_back({g, "kBirthEnergyShare", kBirthEnergyShare});
    out.push_back({g, "kBirthWaterShare", kBirthWaterShare});
    out.push_back({g, "kCourtshipEnergyShare", kCourtshipEnergyShare});
    out.push_back({g, "kNewbornGrowth", kNewbornGrowth});
    out.push_back({g, "kTrampleRate", kTrampleRate});
    out.push_back({g, "kWanderChance", kWanderChance});
    out.push_back({g, "kRoamTicks", static_cast<float>(kRoamTicks)});
    out.push_back({g, "kRoamReach", static_cast<float>(kRoamReach)});
    out.push_back({g, "kMeatPerSize", kMeatPerSize});
    out.push_back({g, "kHuntHunger", kHuntHunger});
    out.push_back({g, "kCarcassRot", kCarcassRot});
    out.push_back({g, "kAttackReach", static_cast<float>(kAttackReach)});
}

} // namespace goblins

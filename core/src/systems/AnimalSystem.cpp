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
#include "core/components/InjuryComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/Body.hpp"
#include "core/Carcass.hpp"
#include "core/Desires.hpp"
#include "core/Diagnostics.hpp"
#include "core/Hunting.hpp"
#include "core/Mating.hpp"
#include "core/Needs.hpp"
#include "core/Scale.hpp"
#include "core/Share.hpp"
#include "core/TileSnapshot.hpp"
#include "core/Walk.hpp"

namespace goblins {

namespace {

// Своей таблицы восьми соседей у системы больше нет: она берёт круговую из
// core/Walk.hpp. Две таблицы с одними и теми же клетками в разном порядке
// уже стоили путаницы — по одной из них считалась разница направлений, где
// порядок значит всё, по другой просто обходились соседи.

// Тела здесь больше нет. Расход на существование, взросление, пищеварение,
// здоровье и смерть — вместе со своими числами (kMinSizeShare,
// kEnergyPerBiomass, kWaterPerFood, kStepEnergy, kDrinkRate, kMinBiteGrowth,
// kDungPeriod, kStarvationHarm, kBirthEnergyShare и прочими) — переехали в
// core/Body.hpp, а смерть и падаль в core/Carcass.hpp. Причина та же, по
// которой в core/Needs.hpp живут голод и жажда: у этого закона стало двое
// вызывающих, и тело у гоблина то же самое, что у животного.
//
// Здесь осталось то, что относится к животному и только к нему: болезнь от
// тесноты сородичей, сторонение чужого вида, пороги желаний, размножение,
// блуждание и охота.

// Стресса за объедание больше нет. Он был второй смертью поверх первой:
// куст и так теряет ровно ту биомассу, которую с него скусили, и стадо,
// пасущееся на месте, и без того держит луг на нуле развитости
// (kMinBiteGrowth ниже не пускает его глубже). Добивать куст ещё и
// смертью от условий значило считать одно и то же дважды — а платила за
// это трава, которая на выпасе не доживала до размера, с которого сеют,
// и участок оставался лысым навсегда. Теперь скушенный куст отрастает,
// как отрастал бы после засухи, и вымирания луга под стадом не случается.

// Болезнь — третья беда того же здоровья, и она единственная приходит не от
// нехватки, а от избытка: чем теснее стоит стадо, тем быстрее по нему идёт
// зараза.
//
// Ради этого она и заведена. Стадо, которому хватает травы, растёт, пока
// не съест луг под корень, и тогда вымирает целиком — половина миров без
// единого хищника кончается именно так. Голод останавливает его слишком
// поздно: он приходит, когда луга уже нет. Болезнь останавливает раньше и
// по другой причине — не "еды не хватило", а "нас стало слишком много на
// одном месте", то есть ровно тем, чем в живом мире и держится численность.
//
// Считается по СВОЕМУ виду, а не по всем травоядным разом: заражается
// подобное подобным. Отсюда даром получается и то, чего в модели не было, —
// повод видам разойтись: сплошной луг одного вида косит сам себя, а
// вперемешку стоящие виды мешают друг другу меньше.
//
// Радиус — не зоркость (genome.perception), а короткое, одинаковое для всех
// расстояние: заражаются от соприкосновения, а не от того, что увидели друг
// друга через полкарты.
//
// kDiseaseCrowd — сколько соседей своего вида ещё НЕ теснота. Порог не
// украшение и не поблажка: заживление упирается в потолок здоровья, болезнь
// вычитается уже после него, а желание пары копится только у целого зверя
// (health >= kFull). Без порога хватало одного соседа, чтобы животное
// никогда больше не захотело потомства, — а сойтись двоим для встречи всё
// равно надо. Поголовье при этом только убывало, без всяких хищников.
//
// Отсюда и вся форма закона: пара — не толпа, мать с телёнком — не толпа, а
// вот шестеро на тринадцати клетках уже теснота. Стадо, разросшееся сверх
// порога, сперва перестаёт плодиться и только потом начинает умирать —
// численность держится тем, что размножение глохнет раньше, чем приходит
// смерть.
constexpr int kDiseaseRadius = 2;
constexpr int kDiseaseCrowd = 3;
constexpr int kDiseaseHarm = 1;

// Насколько травоядное сторонится чужого вида — того, кто ест ту же траву.
// Второй закон про "чужие рядом", парный болезни: та разводит по местности
// своих (теснота одного вида), этот — чужих.
//
// Это НЕ страх, и разница здесь несущая. Страх — желание (Flee), и чтобы
// победить, ему надо перебить и голод, и желание пары, а перебив — оставить
// зверя голодным и без потомства; на тесной карте два вида так заперли бы
// размножение друг другу (та же ловушка, в которую уже попадала болезнь).
// Сторонение живёт этажом ниже желаний — в выборе ноги (core/Walk.hpp,
// WalkShy): зверь не бросает еды и никуда не бежит, он лишь предпочитает ту
// сторону поляны, где чужого нет.
//
// Пробовалось и страхом: kRivalFear = 500 против порога желаний 350 и
// инерции 150. Считать это было незачем — закон не сработал ни разу:
// пасущемуся зверю пришлось бы иметь голод не выше 350 (страх ≥ голод +
// инерция), а голод у травоядного почти всегда выше, потому что в него
// входит и нехватка белка на рост. Виды так и паслись вперемешку.
//
// Вес заведомо меньше kAimPull (1000): животное по-прежнему доходит до
// своей травы, просто подходит к ней с другой стороны. Больше половины
// kAimPull ставить нельзя — тогда чужак начнёт уводить от самой еды.
constexpr int kRivalShyness = 300;

// Желания. Ниже kDesireFloor желание никуда не гонит — животное считается
// довольным и просто бродит. kDesireSwitch — насколько сильнее должно быть
// другое желание, чтобы перебить уже выбранное: без этого запаса животное с
// почти равными голодом и жаждой каждый тик разворачивалось бы и не дошло
// бы ни до травы, ни до воды.
constexpr int kDesireFloor = 350;
constexpr int kDesireSwitch = 150;

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
constexpr int kBreedingGrowth = 900;
constexpr int kCalmNeed = 750;
constexpr int kMateDesire = 600;

// С какой вероятностью ничего не желающее животное всё-таки делает шаг.
// Постоянно бродящее стадо выглядит нервным и зря жжёт энергию; полностью
// неподвижное — мёртвым.
constexpr int kWanderChance = 250;

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
// Отрезок — это направление, а не воображаемая цель где-то впереди
// (прежняя kRoamReach): шаг и так выбирается направлением (core/Walk.hpp),
// и расстоянию до придуманной точки взяться было неоткуда.
constexpr std::uint64_t kRoamTicks = 40;

// --- Хищничество ---

// Голод, с которого выходят на охоту (kHuntHunger), досягаемость зубов
// (kAttackReach) и порог годного мяса (kMinBiteMeat) лежат в
// core/Hunting.hpp вместе с самим выбором добычи: этот закон спрашивает не
// только система, но и наблюдатель — он рисует найденную дорогу на карте.

// --- Падаль пугает ---
// Во что превращается лежащая на клетке туша для травоядного, которое на
// неё набрело. Отсчёт от kMeatPerSize — мяса взрослой туши: целая туша
// пугает в полную силу этого веса, обглоданная и подгнившая — слабее, и
// вместе с мясом страх сходит на нет сам, без отдельного закона забывания.
//
// Меньше единицы намеренно: место, где вчера убили, тревожит, но не гонит
// так, как гонят зубы, которые видно прямо сейчас.
//
// Отдельного невидимого слоя "встревоженности" (DangerComponent) для этого
// больше нет и не нужно: удар зубами и так оставляет на той же клетке
// падаль, и она — тот самый след охоты, только видимый. Два следа одного
// события, ложащиеся на одну клетку и тающие каждый по-своему, — это один
// закон, записанный дважды.
constexpr int kCarcassFearWeight = 700;

// --- Рога и хромота ---
// Удаётся ли жертве достать хищника в ответ, решает её собственная
// меткость (genome->goreChance) — не общее для всего мира число: розыгрыш,
// а не правило, иначе всякий укус рогатого был бы для хищника одинаково
// наказуем, и охота выродилась бы в арифметику.
//
// Насколько долго хромает хищник, получивший рогами в полную силу
// (defense == kFull, взрослая жертва), и во сколько раз он при этом
// медленнее, — края шкалы, по которой едет defense жертвы (см. п.8
// "Зубы"): слабо вооружённый вид калечит короче и слабее, а не только
// короче. 70% скорости при максимальном вложении — заведомо меньше, чем у
// любого травоядного в мире: хромой не догонит никого, пока не заживёт, а
// не настолько беспомощен, чтобы этот срок стал для него смертным
// приговором сам по себе — отходной период после охоты и так самый
// уязвимый момент хищника.
constexpr int kLameMaxTicks = 150;
constexpr int kLameShare = 700;

// --- Намерения ---
// Собираются при обходе животных и исполняются после него. Отдельный шаг
// нужен там же, где и в PlantSystem: на один куст, одну тушу и один
// водопой могут претендовать сразу несколько животных, и решать спор
// порядком обхода Entity нельзя (04_WorldModel.md, п.8).
//
// Само намерение поделиться клеткой и закон дележа живут теперь в
// core/Share.hpp: за один и тот же куст приходят уже не только звери.

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
    int damage = 0;
    // Кто ударил. Нужен затем, что удар бывает обоюдным: рогатая жертва
    // достаёт укусившего в ответ (п.8), и без имени бьющего некому было бы
    // хромать.
    int attacker = 0;
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
    MovementComponent* memory = nullptr;
    InjuryComponent* injury = nullptr;

    // Голод, жажда и страх живут здесь, в снимке тика, а не в компоненте.
    // Все три и раньше пересчитывались из тела и из чужого присутствия
    // каждый тик заново — храниться между тиками им было незачем, и
    // хранились они лишь затем, чтобы их было видно снаружи. Смотреть на
    // них по-прежнему можно (сервер кладёт их в "watched"), но берёт он их
    // теперь оттуда же, откуда и сама система, — из этого снимка.
    int hunger = 0;
    int thirst = 0;
    int fear = 0;
};

// Какое желание сейчас гонит животное.
//
// Сам выбор — общий закон мира (core/Desires.hpp): порог, ниже которого
// желание никуда не гонит, и инерция, с которой уже выбранное держится за
// себя. Здесь остаётся только то, что относится к животному: какие у него
// желания и чем меряется срочность каждого.
//
// Порядок в списке — приоритет при равенстве, побеждает последний. Страх
// поэтому и стоит последним: сытость подождёт, зубы — нет.
Desire chooseDesire(const Animal& animal, bool readyToMate) {
    const DesireComponent& desire = *animal.desire;
    const int mating = readyToMate && desire.mating >= kMateDesire ? desire.mating : 0;

    const Urgency candidates[] = {
        {static_cast<int>(Desire::Food), animal.hunger},
        {static_cast<int>(Desire::Water), animal.thirst},
        {static_cast<int>(Desire::Mate), mating},
        {static_cast<int>(Desire::Flee), animal.fear},
    };

    int currentUrgency = 0;
    switch (desire.current) {
        case Desire::Food: currentUrgency = animal.hunger; break;
        case Desire::Water: currentUrgency = animal.thirst; break;
        case Desire::Mate: currentUrgency = mating; break;
        case Desire::Flee: currentUrgency = animal.fear; break;
        case Desire::Idle: break;
    }

    return static_cast<Desire>(chooseUrgent(candidates, static_cast<int>(desire.current), currentUrgency,
                                            kDesireFloor, kDesireSwitch, static_cast<int>(Desire::Idle)));
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
    // Мутация в тысячных долях вложения (core/Scale.hpp) — а сам расклад
    // бюджета дробный, это генерация, а не состояние мира.
    const float mutationRate = static_cast<float>(worldProperties.animalMutationRate) / kFull;
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
                       InjuryComponent, MovementComponent, PositionComponent>();
    for (const auto entity : animalView) {
        const auto& position = animalView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        animals.push_back(Animal{entity, animalView.get<IdentityComponent>(entity).id, position.x, position.y,
                                  registry.all_of<PredatorComponent>(entity),
                                  &animalView.get<AnimalComponent>(entity),
                                  &animalView.get<AnimalGenomeComponent>(entity),
                                  &animalView.get<DesireComponent>(entity),
                                  &animalView.get<MovementComponent>(entity),
                                  &animalView.get<InjuryComponent>(entity)});
    }

    // Падаль живёт своей жизнью и без животных (гниёт), поэтому выйти
    // раньше времени нельзя — но если нет ни того, ни другого, делать
    // системе действительно нечего.
    const bool anyCarcass = !registry.view<CarcassComponent>().empty();
    if (animals.empty() && !anyCarcass) {
        return;
    }

    // --- 2. Снимок тайлов ---
    // Что лежит на клетках — общий закон снятия (core/TileSnapshot.hpp):
    // им пользуется уже не одна система, и показывать двум системам разный
    // мир нельзя. Снимок снимается ДО того, как хоть одно животное
    // тронулось с места: все решения этого тика принимаются по одному и
    // тому же состоянию мира, и порядок обхода на них не влияет.
    TileSnapshot tiles;
    tiles.capture(world);

    // Дальше по тексту снимок читается по именам его массивов. Ссылки, а не
    // копии, и const — после снятия снимок только читают: изменить в нём
    // клетку значило бы решать этот тик по миру, которого нет.
    const std::vector<entt::entity>& terrain = tiles.terrain;
    const std::vector<int>& waterAt = tiles.waterAt;
    const std::vector<entt::entity>& plantAt = tiles.plantAt;
    const std::vector<int>& plantGrowth = tiles.plantGrowth;
    const std::vector<int>& carcassMeat = tiles.carcassMeat;
    const std::vector<unsigned char>& treeAt = tiles.treeAt;

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
    // Есть ли у страха видимый источник. Страх бывает и беспредметным — от
    // падали под ногами (kCarcassFearWeight), — и тогда бежать НЕ ОТ ЧЕГО:
    // конкретных координат у такого страха нет, а читать их всё равно
    // значило бы бежать в сторону клетки (0, 0).
    std::vector<bool> hasThreat(animals.size(), false);
    // Где ближайший чужой вид травоядных — считается в проходе 3 вместе со
    // страхом, а используется в проходе 4, при выборе шага (kRivalShyness).
    // Живёт здесь по той же причине, что и threatX/threatY: система не
    // хранит состояние между тиками, а животное не помнит того, кого больше
    // не видит.
    std::vector<int> rivalX(animals.size(), 0);
    std::vector<int> rivalY(animals.size(), 0);
    std::vector<bool> hasRival(animals.size(), false);

    // Где стоят хищники — отдельным коротким списком, собранным один раз
    // за тик. Страх ищется перебором, и перебирать весь мир ради горстки
    // зубов дорого не на десятках животных, а на тысячах: перебор "каждое
    // травоядное против всех животных" — это квадрат поголовья, и на пяти
    // тысячах он даёт двадцать пять миллионов проверок за тик, из которых
    // осмысленны сотые доли процента. По списку хищников тот же поиск —
    // "травоядные на хищников", то есть в разы меньше самого квадрата.
    // Тот же приём, что и с preys ниже: собрать один раз, а не заново для
    // каждого.
    std::vector<int> predatorX;
    std::vector<int> predatorY;
    for (const Animal& other : animals) {
        if (other.predator) {
            predatorX.push_back(other.x);
            predatorY.push_back(other.y);
        }
    }

    // --- 3. Тело и желания ---
    // Отдельным проходом от решений (п.4) намеренно: животное, выбирая
    // пару, смотрит, чего хочет сосед, — и если бы желания и решения
    // считались в одном проходе, сосед, которого EnTT хранит позже, был бы
    // ещё с прошлотиковым желанием. Порядок в памяти не может быть причиной
    // события в мире (02_CorePrinciples.md, п.12a), поэтому сначала весь
    // мир узнаёт, чего он хочет, и только потом кто-то что-то делает.
    std::vector<bool> alive(animals.size(), true);
    for (std::size_t a = 0; a < animals.size(); ++a) {
        // Не const: голод, жажда и страх этого тика пишутся сюда же, в
        // снимок, а не в компонент (см. struct Animal).
        Animal& animal = animals[a];
        auto& state = *animal.state;
        const auto& genome = *animal.genome;
        auto& desire = *animal.desire;

        const int size = bodySize(state.growth);

        // Что время сделало с телом: расход на существование, взросление,
        // пищеварение, здоровье. Закон общий для всего живого и живёт в
        // core/Body.hpp — тело у гоблина то же самое.
        advanceBody(state, genome, tick, animal.id);

        // Болезнь: чем теснее вокруг стоят свои, тем быстрее по стаду идёт
        // зараза (см. kDiseaseHarm). Не решение животного и не желание, а
        // то же здоровье, что отнимают голод и жажда, — поэтому и считается
        // здесь, в теле, а не среди желаний.
        //
        // Своих — значит своего вида и своей диеты. Перебор по всем
        // животным того же тика: их десятки, и обходится он дешевле, чем
        // просмотр клеток вокруг (тот же приём, что у страха ниже).
        //
        // Хищников это не касается: их в мире единицы, тесноты у них не
        // бывает, а численность им держит не зараза, а голод.
        if (!animal.predator) {
            int crowd = 0;
            for (std::size_t b = 0; b < animals.size(); ++b) {
                if (b == a || animals[b].predator || animals[b].genome->species != genome.species) {
                    continue;
                }
                const int dx = animals[b].x - animal.x;
                const int dy = animals[b].y - animal.y;
                if (dx * dx + dy * dy <= kDiseaseRadius * kDiseaseRadius) {
                    ++crowd;
                }
            }
            // Считается только теснота СВЕРХ kDiseaseCrowd: пара — не толпа,
            // и мать с телёнком тоже. Порог здесь не смягчение, а условие
            // того, чтобы стадо вообще могло размножаться: заживление
            // упирается в потолок здоровья, болезнь вычитается уже после
            // него, а желание пары копится только у целого зверя
            // (kFull, см. content ниже). Без порога один-единственный сосед
            // навсегда лишал животное потомства — а сойтись для встречи
            // двоим всё равно надо, и оба тут же переставали хотеть пары.
            // Поголовье от этого могло только убывать, без всяких хищников.
            const int crush = crowd - kDiseaseCrowd;
            if (crush > 0) {
                state.health -= kDiseaseHarm * crush;
            }
        }

        // Смерть от старости, от условий или от чужих зубов. Проверяется
        // здесь, а не внутри advanceBody, и это не мелочь: болезнь отнимает
        // здоровье уже после общего закона тела, и умереть от неё животное
        // обязано в тот же тик, а не в следующий.
        //
        // Entity исчезает не сейчас, а при разрешении очереди команд
        // (05_Entity.md, п.5), и тело ложится падалью — одинаково, от чего
        // бы животное ни умерло.
        if (bodyDied(state, genome)) {
            enqueueDeath(commands, animal.entity, animal.x, animal.y);
            alive[a] = false;
            continue;
        }

        // Память ног тает со временем, а не от шагов (core/Walk.hpp): зверь,
        // простоявший сотню тиков у куста, не должен помнить преграду,
        // которой давно нет.
        fadeWalkMemory(*animal.memory);

        // Хромота заживает сама, по тику за тик (см. InjuryComponent). Срок,
        // а не скорость: дробям в состоянии мира места нет.
        //
        // lameShare сбрасывается ровно тогда, когда срок истёк, а не
        // раньше и не сам по себе: пока хищник ещё хромает, тяжесть должна
        // оставаться той, что назначил укус (min с новым ударом — берём
        // худшее из двух, см. п.8 "Зубы"). Без сброса тяжесть только
        // копилась бы: min никогда не отпускает вверх, и однажды
        // сильно потрёпанный рогами хищник оставался бы почти неподвижным
        // при КАЖДОЙ следующей хромоте до конца жизни, даже от слабого
        // укуса, — раненый однажды не должен хромать хуже раненого только
        // что.
        if (animal.injury->lameTicks > 0) {
            --animal.injury->lameTicks;
            if (animal.injury->lameTicks == 0) {
                animal.injury->lameShare = kFull;
            }
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
        animal.hunger = hungerOf(state, genome);
        animal.thirst = thirstOf(state, genome);

        // Страх: насколько близко видна опасность. Хищник ни от кого не
        // бегает — на него в этом мире не охотятся (09_Animals.md, п.2).
        animal.fear = 0;
        if (!animal.predator) {
            const int sightCells = std::max(1, genome.perception);
            const float sight = static_cast<float>(sightCells);
            for (std::size_t b = 0; b < predatorX.size(); ++b) {
                const int dx = predatorX[b] - animal.x;
                const int dy = predatorY[b] - animal.y;
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
                // Дробное расстояние тут же становится целым страхом:
                // считать корень из суммы квадратов целыми числами незачем,
                // а хранится всё равно целое (core/Scale.hpp).
                const int scare =
                    kDesireFloor + static_cast<int>((kFull - kDesireFloor) * (1.0f - distance / sight));
                if (scare > animal.fear) {
                    animal.fear = scare;
                    threatX[a] = predatorX[b];
                    threatY[a] = predatorY[b];
                    hasThreat[a] = true;
                }
            }

            // Чужой вид травоядных: не зубы, но и не сородич — он ест ту
            // же траву. Убегать от него незачем (и незачем ради этого
            // бросать еду), а вот стоять с ним нос к носу не за чем:
            // запоминаем, с какой стороны ближайший, и сторонимся уже на
            // ходу — см. "Шаг" ниже и kRivalShyness.
            //
            // Свой вид сюда не попадает намеренно: сторониться своих значило
            // бы разогнать собственное стадо и остаться без пары. Что делает
            // с чужими СВОЙ вид, сказано отдельно и по-другому — это болезнь
            // от тесноты (kDiseaseCrowd выше), то есть тело, а не походка.
            //
            // Перебор по всем животным, а не по короткому списку, как у
            // хищников: чужой вид — это почти всё поголовье травоядных, и
            // выигрывать тут нечего.
            int rivalDistance = 0;
            for (std::size_t b = 0; b < animals.size(); ++b) {
                if (b == a || animals[b].predator || animals[b].genome->species == genome.species) {
                    continue;
                }
                const int dx = animals[b].x - animal.x;
                const int dy = animals[b].y - animal.y;
                const int distance = dx * dx + dy * dy;
                if (distance > sightCells * sightCells) {
                    continue;
                }
                if (hasRival[a] && distance >= rivalDistance) {
                    continue;
                }
                rivalX[a] = animals[b].x;
                rivalY[a] = animals[b].y;
                hasRival[a] = true;
                rivalDistance = distance;
            }

            // Падаль под ногами: здесь кого-то убили и съели, и это тоже
            // страшно — пусть слабее, чем видимые зубы. Туша и есть след
            // охоты, только видимый: отдельного невидимого слоя
            // "встревоженности" для того же самого больше нет (см.
            // kCarcassFearWeight).
            //
            // У такого страха нет источника, от которого убегать: животное
            // не знает, где хищник, оно знает только, что тут плохое место.
            // Поэтому threatX/threatY не трогаем — бегущий без цели просто
            // уходит куда глаза глядят (см. "Шаг"), и уже этим освобождает
            // участок, а хищнику приходится искать добычу заново в другом
            // месте.
            const int dread =
                std::min(kFull, carcassMeat[index(animal.x, animal.y)] * kCarcassFearWeight / kMeatPerSize);
            if (dread > animal.fear) {
                animal.fear = dread;
            }
        }

        const bool adult = state.age >= genome.maturityAge && state.growth >= kBreedingGrowth;
        // Не бедствует: ни голод, ни жажда не дошли до предела, и нет
        // накопленного стресса — то есть в последние десятки тиков запасы
        // не кончались совсем.
        const bool content = state.health >= kFull && animal.hunger < kCalmNeed && animal.thirst < kCalmNeed;
        if (adult && content) {
            // Желание пары — единственное, которого в теле не прочитать: оно
            // копится со временем у того, кому больше нечего хотеть.
            desire.mating = std::min(kFull, desire.mating + genome.breedingUrge);
        }
        desire.current = chooseDesire(animal, adult && content);
    }

    // Добыча в том виде, в каком её видит хищник (core/Hunting.hpp): где
    // стоит и как быстро бежит. Собирается один раз на тик, а не заново для
    // каждого хищника, и рядом с ней — обратный указатель в снимок: закон
    // охоты возвращает номер в этом списке, а бить надо конкретное животное.
    std::vector<HuntPrey> preys;
    std::vector<int> preyOwner;
    for (std::size_t b = 0; b < animals.size(); ++b) {
        if (!alive[b] || animals[b].predator) {
            continue;
        }
        preys.push_back(HuntPrey{animals[b].x, animals[b].y, animals[b].genome->speed,
                                  animals[b].genome->defense,
                                  treeAt[index(animals[b].x, animals[b].y)] != 0});
        preyOwner.push_back(static_cast<int>(b));
    }

    // Возможная пара в том виде, в каком её видно со стороны
    // (core/Mating.hpp): кто такой и согласен ли сойтись. Список один на
    // всех ищущих и на обе диеты — кому кто ровня, разбирается сам закон.
    // Желания у всех уже посчитаны (п.3), поэтому "согласен" здесь честное,
    // а не прошлотиковое.
    std::vector<MateCandidate> mates;
    for (std::size_t b = 0; b < animals.size(); ++b) {
        mates.push_back(MateCandidate{animals[b].id, animals[b].x, animals[b].y, animals[b].genome->species,
                                       animals[b].predator, animals[b].state->sex,
                                       alive[b] && animals[b].desire->current == Desire::Mate});
    }

    // Округа зверя и дорога по ней (core/Path.hpp). Живут снаружи цикла и
    // переиспользуются: за тик волна пускается столько раз, сколько в мире
    // ищущих, а массивы у неё на всю Область.
    Reach reachOf;
    std::vector<PathCell> road;

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
        const int size = bodySize(state.growth);

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
        const int reach = std::max(1, genome.perception);
        bool busy = false;
        bool hasTarget = false;
        int targetX = animal.x;
        int targetY = animal.y;

        // Хромой ходит медленнее — и это ЕДИНСТВЕННОЕ, что делает с ним
        // увечье (см. InjuryComponent). Больше ничего и не нужно: скорость
        // решает и погоню, и бегство, а половины хода не хватает, чтобы
        // догнать хоть кого-нибудь.
        //
        // Одна величина на оба места, где скорость вообще участвует: и на
        // сам шаг, и на выбор добычи. Выбор — не косметика: хищник
        // пропускает тех, кто не медленнее его (core/Hunting.hpp), и если
        // хромота не войдёт в это сравнение, он побежит за той добычей,
        // догнать которую уже не может, и будет жечь силы впустую.
        const int effectiveSpeed = animal.injury->lameTicks > 0
                                        ? genome.speed * animal.injury->lameShare / kFull
                                        : genome.speed;

        // Куда животное вообще может встать (core/Hunting.hpp, standableAt):
        // не за границей Области, не на занятый непроходимым объектом тайл и
        // не в воду. Само правило — там, здесь только факты, из которых оно
        // складывается: снимок тайлов этого тика.
        // Край Области проверяется здесь же, а не оставляется вызывающим:
        // спрашивают эту годность и по клеткам вокруг (шаг — core/Walk.hpp),
        // и по кругу видимости (дорога — core/Path.hpp), и за краем карты
        // читать нечего — там нет ни почвы, ни воды, а есть чужая память.
        auto standable = [&](int nx, int ny) {
            if (!world.area().inBounds(nx, ny)) {
                return false;
            }
            const std::size_t cell = index(nx, ny);
            return standableAt(world.area().isBlocked(nx, ny), terrain[cell] != entt::null, waterAt[cell]);
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
            int bestDistance = 0;
            int ties = 0;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int nx = animal.x + dx;
                    const int ny = animal.y + dy;
                    if (!world.area().inBounds(nx, ny)) {
                        continue;
                    }
                    const int distance = dx * dx + dy * dy;
                    if (distance > reach * reach) {
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
                        if (randomBelow(random, static_cast<std::uint64_t>(ties)) != 0) {
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
            return ties > 0 ? bestDistance : -1;
        };

        switch (desire.current) {
            case Desire::Food: {
                if (animal.predator) {
                    // Хищник ест не жертву, а падаль: сначала надо убить
                    // (или найти уже мёртвое), и только потом есть.
                    if (carcassMeat[here] > kMinBiteMeat) {
                        meals.push_back(
                            ShareIntent{here, static_cast<int>(a), animal.id, genome.biteSize * size / kFull});
                        busy = true;
                        break;
                    }

                    // --- Дорога ---
                    // Прежде чем выбирать, за кем гнаться, хищник
                    // прикидывает дорогу: волна расходится от него самого
                    // по проходимым клеткам и считает, за сколько шагов он
                    // доберётся до каждой. Куда волна не пришла, туда хода
                    // нет — там вода, камень или чужой берег. Сам закон
                    // (кого выбрать и какой дорогой идти) живёт в
                    // core/Hunting.hpp: по нему же наблюдатель рисует эту
                    // дорогу на карте, и разъехаться им негде.
                    reachOf.build(world.area(), animal.x, animal.y, reach, standable);
                    const HuntChoice choice = chooseHuntTarget(
                        reachOf, Hunter{animal.x, animal.y, reach, effectiveSpeed, animal.hunger}, preys,
                        [&](int nx, int ny) { return carcassMeat[index(nx, ny)]; }, random);

                    // Добыча в пределах досягаемости зубов — бьём, и никуда
                    // при этом не идём.
                    if (choice.kind == HuntChoice::Kind::Prey && choice.atTeeth) {
                        attacks.push_back(AttackIntent{preyOwner[static_cast<std::size_t>(choice.prey)],
                                                       genome.attack * size / kFull, static_cast<int>(a)});
                        busy = true;
                        break;
                    }

                    // Цель шага — не сама добыча и не туша, а следующая
                    // клетка дороги к ним: идти хищник обязан по дороге, а
                    // не напролом. Напролом он упрётся ровно в тот берег,
                    // который дорога и обходит, и вся находка пропадёт зря.
                    if (choice.kind != HuntChoice::Kind::None) {
                        reachOf.roadTo(choice.x, choice.y, road);
                        if (!road.empty()) {
                            targetX = road.front().x;
                            targetY = road.front().y;
                            hasTarget = true;
                        }
                    }
                    break;
                }

                if (plantAt[here] != entt::null && plantGrowth[here] > kMinBiteGrowth) {
                    bites.push_back(
                        ShareIntent{here, static_cast<int>(a), animal.id, genome.biteSize * size / kFull});
                    busy = true;
                } else {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return plantAt[cell] != entt::null && plantGrowth[cell] > kMinBiteGrowth &&
                                               standable(nx, ny);
                                    },
                                    targetX, targetY) >= 0;
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
                if (waterAt[here] > 0) {
                    source = here;
                } else {
                    for (int dir = 0; dir < 8; ++dir) {
                        const int nx = animal.x + kWalkX[dir];
                        const int ny = animal.y + kWalkY[dir];
                        if (!world.area().inBounds(nx, ny)) {
                            continue;
                        }
                        const std::size_t j = index(nx, ny);
                        if (waterAt[j] > 0) {
                            source = j;
                            break;
                        }
                    }
                }
                if (source < cellCount) {
                    drinks.push_back(
                        ShareIntent{source, static_cast<int>(a), animal.id, kDrinkRate * size / kFull});
                    busy = true;
                } else {
                    hasTarget =
                        findNearest([&](std::size_t cell, int, int) { return waterAt[cell] > 0; }, targetX,
                                     targetY) >= 0;
                }
                break;
            }
            case Desire::Mate: {
                // Пару ищут той же дорогой, что хищник ищет добычу
                // (core/Mating.hpp): увиденное через реку — ещё не
                // найденное. Пара за водой видна обоим, сойтись им негде, и
                // оба стоят — самка ждёт на месте, самец упирается в берег;
                // так и проходит остаток их жизни в двадцати шагах друг от
                // друга.
                //
                // Волна считается только тогда, когда есть на кого смотреть:
                // перебор животных дёшев, а волна по округе — нет, и платить
                // за неё каждым ищущим зверем каждый тик незачем.
                const Suitor suitor{animal.id,      animal.x,        animal.y, reach,
                                    genome.species, animal.predator, state.sex};
                if (!anyMateInSight(suitor, mates)) {
                    // Рядом никого не видно — но самка, которой нужна пара,
                    // слышна дальше, чем видна (core/Mating.hpp, hearCall).
                    // Цель ставится прямо, без дороги: звук не спрашивает
                    // брода, и как до зовущей дойти, решит уже сам шаг ниже
                    // — тем же способом, каким слепое блуждание само огибает
                    // преграды. Без этого поголовье, разбросанное по
                    // большой карте, вымирало не от голода и не от зубов, а
                    // от одиночества: предпоследняя пара уходила каждый в
                    // свою случайную сторону и расходилась дальше, а не
                    // ближе.
                    const MateChoice call = hearCall(suitor, mates);
                    if (call.found) {
                        targetX = call.x;
                        targetY = call.y;
                        hasTarget = true;
                    }
                    break;
                }
                reachOf.build(world.area(), animal.x, animal.y, reach, standable);
                const MateChoice mate = chooseMate(reachOf, suitor, mates);
                if (!mate.found) {
                    break;
                }

                // Сошлись — встреча случилась на этой клетке. Кто с кем
                // именно, решится ниже (п.10), когда соберутся все:
                // намерение здесь не называет второго, потому что на одной
                // клетке их может ждать и трое.
                if (mate.x == animal.x && mate.y == animal.y) {
                    matings.push_back(MateIntent{here, static_cast<int>(a), animal.id, genome.species,
                                                  animal.predator, state.sex});
                    busy = true;
                    break;
                }

                // Навстречу идёт только самец, самка при виде него ждёт на
                // месте. Это не украшение поведения, а необходимость: два
                // животных, шагающих друг к другу, каждый тик меняются
                // клетками и остаются соседями — они могут так и не
                // встретиться, продолжая всю жизнь ходить друг сквозь
                // друга. Кто именно ждёт, мир решает полом, а не жребием:
                // жребий пришлось бы бросать заново каждый тик, и пара
                // снова начала бы топтаться.
                //
                // Ждёт она теперь только того, до кого есть дорога: раньше
                // самка садилась ждать всякого, кого увидит, — в том числе
                // того, кто до неё никогда не дойдёт.
                if (state.sex == Sex::Female) {
                    busy = true;
                    break;
                }

                reachOf.roadTo(mate.x, mate.y, road);
                if (!road.empty()) {
                    targetX = road.front().x;
                    targetY = road.front().y;
                    hasTarget = true;
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

        // Скорость — тысячных клетки за тик (core/Scale.hpp): копится,
        // пока не наберётся целая клетка, и тогда животное переставляет
        // ноги. Одна клетка за тик и не больше, даже если скорость выше
        // единицы (у хищника она до 1800).
        //
        // Отсюда и потолок в две клетки: у того, кто быстрее клетки за
        // тик, невыбранный запас иначе растёт без конца — шагов от этого
        // больше не становится, но число в теле уезжает в бесконечность.
        // Дробным оно уезжало туда же, просто этого не было видно; целое
        // состояние мира обязано жить в тех пределах, которые о нём
        // объявлены (см. AnimalComponent::stepProgress).
        state.stepProgress = std::min(state.stepProgress + effectiveSpeed, 2 * kFull - 1);
        if (state.stepProgress < kFull) {
            continue;
        }
        state.stepProgress -= kFull;

        const bool fleeing = desire.current == Desire::Flee;

        // Направление поиска, когда желаемого не видно. Ищущее животное
        // идёт в одну сторону целый отрезок пути (kRoamTicks), а не
        // топчется на месте: за пределами собственной видимости другого
        // способа что-нибудь найти у него нет. Берётся из постоянного
        // идентификатора и номера отрезка, поэтому системе не нужно ничего
        // помнить между тиками, а животное всё равно идёт в одну сторону,
        // пока отрезок не сменится.
        auto roamDirection = [&]() {
            std::uint64_t roam = mixSeed(animal.id, tick / kRoamTicks);
            return static_cast<int>(nextState(roam) % 8u);
        };

        // Куда животное хочет — ОДНО направление на все случаи движения, и
        // дальше шаг считается для всех одинаково (core/Walk.hpp). Идущий к
        // цели, бегущий от зубов, ищущий за пределами видимости и просто
        // бродящий отличаются только тем, откуда взялось это направление.
        //
        // Прежде здесь было три разных перебора соседей — "ближе к цели",
        // "дальше от опасности" и "первый попавшийся", — и к ним обход
        // преграды отдельным случаем. Обход теперь получается сам: если
        // прямо на цель пройти нельзя, лучшим оказывается шаг вбок, а
        // память ног не даёт вернуться назад тем же путём.
        int aim = -1;
        if (fleeing) {
            // Бежать есть куда: под деревом добычу не высматривают
            // (kCoverSight, core/Hunting.hpp). Испуганный ищет глазами
            // ближайшую крону в пределах своей зоркости и правит туда — а
            // не просто прочь. Именно это и делает рощу убежищем: не тем,
            // что она защищает того, кто там оказался, а тем, что к ней
            // бегут.
            //
            // Уже стоящему под кроной бежать некуда и незачем: он на месте
            // и есть.
            const int sight = std::max(1, genome.perception);
            int coverX = 0;
            int coverY = 0;
            int coverSteps = -1;
            if (treeAt[index(animal.x, animal.y)] == 0) {
                for (int dy = -sight; dy <= sight; ++dy) {
                    for (int dx = -sight; dx <= sight; ++dx) {
                        const int nx = animal.x + dx;
                        const int ny = animal.y + dy;
                        if (!world.area().inBounds(nx, ny) || treeAt[index(nx, ny)] == 0) {
                            continue;
                        }
                        if (dx * dx + dy * dy > sight * sight) {
                            continue;
                        }
                        const int steps = std::max(std::abs(dx), std::abs(dy));
                        if (coverSteps < 0 || steps < coverSteps) {
                            coverSteps = steps;
                            coverX = nx;
                            coverY = ny;
                        }
                    }
                }
            }

            if (coverSteps >= 0) {
                aim = walkDirectionTo(animal.x, animal.y, coverX, coverY);
            }

            // Кроны рядом нет — тогда как и прежде: от опасности не идут к
            // цели, от неё уходят, и направление берётся от неё к себе.
            //
            // Но опасность бывает и беспредметной: встревоженная земля
            // пугает, не показывая, откуда ждать зубов (см. п.3). Убегать
            // от неё некуда — от неё уходят, всё равно куда, и уходят тем
            // же отрезком поиска, каким ищут невидимое. Не будь этого,
            // животное бежало бы от клетки (0, 0), то есть всегда в один и
            // тот же угол мира.
            if (aim < 0) {
                aim = hasThreat[a] ? walkDirectionTo(threatX[a], threatY[a], animal.x, animal.y) : -1;
            }
            if (aim < 0) {
                // Либо страх беспредметен, либо хищник стоит ровно на той
                // же клетке: и там, и там — куда угодно, лишь бы не стоять.
                aim = roamDirection();
            }
        } else if (hasTarget) {
            aim = walkDirectionTo(animal.x, animal.y, targetX, targetY);
        } else if (desire.current == Desire::Food || desire.current == Desire::Water ||
                   desire.current == Desire::Mate) {
            // Желаемого не видно — значит, надо искать. Ищут все трое: и
            // еду, и воду, и пару; река, до которой не дошли, убивает
            // вернее любых зубов.
            aim = roamDirection();
        } else if (static_cast<int>(randomBelow(random, kFull)) >= kWanderChance) {
            continue; // ничего не гонит — стоит и щиплет что придётся
        }

        // Сам шаг: восемь соседей, тяга к выбранной стороне, инерция,
        // память о своём следе и о преградах, щепоть случайности. На клетку
        // с травой, с падалью и с другим животным идут свободно — заняты
        // для животного только вода и камень (core/Path.hpp).
        //
        // Кого сторониться: травоядное — ближайшего чужого вида (п.3 выше).
        // Не бросая при этом ни еды, ни пути к паре: сторонение только
        // выбирает, с какой стороны подойти (core/Walk.hpp, WalkShy).
        const WalkShy shy =
            hasRival[a] ? WalkShy{walkDirectionTo(animal.x, animal.y, rivalX[a], rivalY[a]), kRivalShyness}
                        : WalkShy{};
        const WalkStep step = chooseStep(*animal.memory, animal.x, animal.y, aim, shy, standable, random);
        if (!step.moved) {
            continue; // шагнуть некуда вовсе: вода, камень или край мира
        }

        state.energy = std::max(0, state.energy - kStepEnergy * size / kFull);
        steps.push_back(StepIntent{static_cast<int>(a), step.x, step.y});
    }

    // --- 5. Кормёжка травоядных: один куст на всех, кто до него дотянулся ---
    // Спор делится долями: если куста на всех не хватает, каждый получает
    // свою часть. Порядок внутри клетки — по постоянному идентификатору, а
    // не по порядку в хранилище: крупицы белка целые, и кому достанется
    // последняя, решает мир, а не EnTT.
    std::sort(bites.begin(), bites.end(), sortByCellThenId);
    for (std::size_t n = 0; n < bites.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < bites.size() && bites[m].cell == bites[n].cell) {
            demand += bites[m].want;
            ++m;
        }

        const entt::entity plantEntity = plantAt[bites[n].cell];
        auto* plant = registry.valid(plantEntity) ? registry.try_get<PlantComponent>(plantEntity) : nullptr;
        if (plant == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        const int growthBefore = plant->growth;
        const int mineralsBefore = plant->minerals;

        // Крупицы белка делятся между едоками целочисленно и без остатка:
        // каждому достаётся столько, сколько причитается на всё съеденное
        // им И теми, кто был до него, минус уже розданное. Накопителя доли
        // (proteinPending) при этом не нужно ни одному из них — куст
        // отдаёт свои крупицы здесь и сейчас, ровно по разу каждую.
        int eatenTotal = 0;
        int releasedTotal = 0;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(bites[k].want, growthBefore, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *animals[static_cast<std::size_t>(bites[k].claimant)].state;
            const auto& genome = *animals[static_cast<std::size_t>(bites[k].claimant)].genome;

            feedBody(state, genome, eaten);
            eatenTotal += eaten;

            // Крупицы белка достаются едоку целыми: сколько причитается на
            // всё съеденное им И теми, кто был до него, минус уже
            // розданное. Накопителя доли (proteinPending) при этом не нужно
            // ни одному из них — куст отдаёт свои крупицы здесь и сейчас,
            // ровно по разу каждую.
            const int owed =
                growthBefore > 0 ? std::min(mineralsBefore, mineralsBefore * eatenTotal / growthBefore) : 0;
            int taken = 0;
            for (int grain = releasedTotal; grain < owed && plant->minerals > 0; ++grain) {
                --plant->minerals;
                ++taken;
            }
            takeProtein(state, genome, taken);
            releasedTotal = std::max(releasedTotal, owed);
        }

        // Объедание отнимает биомассу, и только её: погибнуть от зубов куст
        // не может, отрастёт он или нет — решит PlantSystem по своим
        // законам на следующем тике.
        plant->growth = std::max(0, plant->growth - eatenTotal);
        n = m;
    }

    // --- 6. Кормёжка хищников: одна туша на всех, кто до неё добрался ---
    // Тот же закон дележа, что и у травы: спор решается долями, а целые
    // крупицы белка расходятся по порядку идентификаторов.
    std::sort(meals.begin(), meals.end(), sortByCellThenId);
    for (std::size_t n = 0; n < meals.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < meals.size() && meals[m].cell == meals[n].cell) {
            demand += meals[m].want;
            ++m;
        }

        const entt::entity tile = terrain[meals[n].cell];
        auto* carcass =
            tile != entt::null && registry.valid(tile) ? registry.try_get<CarcassComponent>(tile) : nullptr;
        if (carcass == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        const int meatBefore = carcass->meat;

        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(meals[k].want, meatBefore, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *animals[static_cast<std::size_t>(meals[k].claimant)].state;
            const auto& genome = *animals[static_cast<std::size_t>(meals[k].claimant)].genome;

            feedBody(state, genome, eaten);

            const int meatNow = carcass->meat;
            carcass->meat = std::max(0, carcass->meat - eaten);
            takeProtein(state, genome, releaseCarcassProtein(*carcass, meatNow, meatNow - carcass->meat));
        }
        n = m;
    }

    // --- 7. Водопой: так же долями ---
    std::sort(drinks.begin(), drinks.end(), sortByCellThenId);
    for (std::size_t n = 0; n < drinks.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < drinks.size() && drinks[m].cell == drinks[n].cell) {
            demand += drinks[m].want;
            ++m;
        }

        const std::size_t cell = drinks[n].cell;
        const entt::entity tile = terrain[cell];
        auto* water = tile != entt::null && registry.valid(tile) ? registry.try_get<WaterComponent>(tile) : nullptr;
        if (water == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        // Стадо пьёт из клетки, но не вычерпывает её: глубина воды здесь не
        // трогается вовсе. Река живёт по своему закону (HydrologySystem) —
        // течение, испарение, дожди, — и водопой в этот баланс не входит, а
        // спор за один водопой решается тем, что все пьющие делят одну
        // клетку, а не тем, насколько она от этого просела.
        for (std::size_t k = n; k < m; ++k) {
            auto& state = *animals[static_cast<std::size_t>(drinks[k].claimant)].state;
            const auto& genome = *animals[static_cast<std::size_t>(drinks[k].claimant)].genome;
            state.water = std::min(genome.waterCapacity, state.water + drinks[k].want);
        }
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

        // Рога. Жертва, у которой есть чем бодаться, достаёт укусившего в
        // ответ — не всегда, а как повезёт: попадает ли удар вообще, решает
        // её же меткость (genome->goreChance), а не общее для всего мира
        // число. Удар по рогам не отнимает у хищника здоровья, но оставляет
        // его хромым, и на это время он перестаёт кого-либо догонять (см.
        // InjuryComponent).
        //
        // Розыгрыш собирается из seed мира, тика и имён обоих зверей
        // (core/Random.hpp): системе не нужно ничего помнить, а разные пары
        // в один тик получают разный исход.
        const auto attacker = static_cast<std::size_t>(attack.attacker);
        const int defense = animals[prey].genome->defense;
        if (defense > 0 && alive[attacker]) {
            std::uint64_t gore =
                mixSeed(animalSeed, mixSeed(tick, mixSeed(animals[prey].id, animals[attacker].id)));
            if (static_cast<int>(randomBelow(gore, kFull)) < animals[prey].genome->goreChance) {
                // Мелкий бодает слабее взрослого — тем же размером, каким
                // считается и всё остальное в теле.
                const int preySize = bodySize(animals[prey].state->growth);
                // Оба измерения хромоты — из одного и того же defense: срок
                // (как раньше) и теперь ещё тяжесть. Слабо вооружённый вид
                // калечит слабее, а не только короче — kLameShare здесь не
                // плоское значение, а худший край шкалы, к которому лишь
                // приближается защита неполного вложения.
                const int lame = kLameMaxTicks * defense / kFull * preySize / kFull;
                const int lameShare = kFull - (kFull - kLameShare) * defense / kFull * preySize / kFull;
                if (lame > 0) {
                    auto& injury = *animals[attacker].injury;
                    // Не складывается: два укуса подряд не удваивают срок и
                    // не удваивают тяжесть, а оставляют больший срок и
                    // больший вред. Хромота — состояние, а не счётчик
                    // полученных ударов.
                    injury.lameTicks = std::max(injury.lameTicks, lame);
                    injury.lameShare = std::min(injury.lameShare, lameShare);
                }
            }
        }
    }
    for (std::size_t a = 0; a < animals.size(); ++a) {
        if (!alive[a] || animals[a].state->health > 0) {
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
    }

    // Вытаптывания под ногами больше нет: утоптанность ушла из почвы
    // вместе с пригодностью, которую она кормила (см. SoilComponent).
    // Обратная связь стада на луг осталась одна, зато прямая — поедание:
    // скушенный куст стоит объеденным, пока стадо рядом, и тропа к водопою
    // вытаптывается зубами, а не ногами.

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
        calf.sex = randomBelow(random, 2) == 0 ? Sex::Female : Sex::Male;

        // Детёныш появляется не из ниоткуда: и запасы, и крупица белка —
        // материнские. Ровно тот же обмен, что у растения с семенем.
        const int givenEnergy = mother.state->energy * kBirthEnergyShare / kFull;
        mother.state->energy -= givenEnergy;
        calf.energy = std::min(givenEnergy, childGenome.energyCapacity);

        const int givenWater = mother.state->water * kBirthWaterShare / kFull;
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
        calf.growth = childGenome.proteinNeed > 0
                           ? std::min(calf.growth, calf.protein * kFull / childGenome.proteinNeed)
                           : calf.growth;

        mother.desire->mating = 0;
        father.desire->mating = 0;
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
            // хотеть, ему скажет первый же тик (см. п.3 выше). И ногами он
            // пока ничего не помнит: ни шага, ни преграды.
            w.registry().emplace<DesireComponent>(entity, DesireComponent{});
            w.registry().emplace<MovementComponent>(entity);
            w.registry().emplace<InjuryComponent>(entity);
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

        const int meatBefore = carcass.meat;
        carcass.meat = std::max(0, carcass.meat - kCarcassRot);
        int toHumus = releaseCarcassProtein(carcass, meatBefore, meatBefore - carcass.meat);

        if (carcass.meat <= 0) {
            // Мяса не осталось — весь недоеденный белок уходит в землю
            // разом: держать его в пустой туше не за что.
            toHumus += carcass.protein;
            carcass.protein = 0;
        }

        if (toHumus > 0) {
            commands.enqueue([x = position.x, y = position.y, toHumus](World& w) { depositHumus(w, x, y, toHumus); });
        }

        if (carcass.meat <= 0) {
            commands.enqueue([entity](World& w) {
                // Проверяем заново, а не полагаемся на снятое выше
                // состояние: пока команда ждала очереди, на этой же клетке
                // могло умереть ещё одно животное, и в туше снова есть мясо.
                auto* remains = w.registry().try_get<CarcassComponent>(entity);
                if (remains != nullptr && remains->meat <= 0 && remains->protein <= 0) {
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
    out.push_back({g, "kWaterPerFood", kWaterPerFood});
    out.push_back({g, "kStepEnergy", kStepEnergy});
    out.push_back({g, "kDrinkRate", kDrinkRate});
    out.push_back({g, "kMinBiteGrowth", kMinBiteGrowth});
    out.push_back({g, "kMinBiteMeat", kMinBiteMeat});
    out.push_back({g, "kDungPeriod", static_cast<float>(kDungPeriod)});
    out.push_back({g, "kDungDrop", static_cast<float>(kDungDrop)});
    out.push_back({g, "kStarvationHarm", kStarvationHarm});
    out.push_back({g, "kDehydrationHarm", kDehydrationHarm});
    out.push_back({g, "kRecoveryRate", kRecoveryRate});
    out.push_back({g, "kDesireFloor", kDesireFloor});
    out.push_back({g, "kDesireSwitch", kDesireSwitch});
    out.push_back({g, "kBreedingGrowth", kBreedingGrowth});
    out.push_back({g, "kCalmNeed", kCalmNeed});
    out.push_back({g, "kMateDesire", kMateDesire});
    out.push_back({g, "kCallRange", static_cast<float>(kCallRange)});
    out.push_back({g, "kBirthEnergyShare", kBirthEnergyShare});
    out.push_back({g, "kBirthWaterShare", kBirthWaterShare});
    out.push_back({g, "kNewbornGrowth", kNewbornGrowth});
    out.push_back({g, "kWanderChance", kWanderChance});
    out.push_back({g, "kRoamTicks", static_cast<float>(kRoamTicks)});
    out.push_back({g, "kMeatPerSize", kMeatPerSize});
    out.push_back({g, "kHuntHunger", kHuntHunger});
    out.push_back({g, "kCarcassRot", kCarcassRot});
    out.push_back({g, "kAttackReach", static_cast<float>(kAttackReach)});
    out.push_back({g, "kHuntCaution", static_cast<float>(kHuntCaution)});
    out.push_back({g, "kDiseaseRadius", static_cast<float>(kDiseaseRadius)});
    out.push_back({g, "kDiseaseCrowd", static_cast<float>(kDiseaseCrowd)});
    out.push_back({g, "kDiseaseHarm", kDiseaseHarm});
    out.push_back({g, "kRivalShyness", kRivalShyness});
    out.push_back({g, "kCarcassFearWeight", kCarcassFearWeight});
    out.push_back({g, "kLameMaxTicks", static_cast<float>(kLameMaxTicks)});
    out.push_back({g, "kLameShare", kLameShare});

    // Веса шага (core/Walk.hpp) — своей группой: они не про жизнь животного,
    // а про его походку, и подбираются вместе, друг против друга.
    constexpr const char* w = "Animals (walk)";
    out.push_back({w, "kTrailSteps", static_cast<float>(kTrailSteps)});
    out.push_back({w, "kAimPull", kAimPull});
    out.push_back({w, "kInertiaPull", kInertiaPull});
    out.push_back({w, "kTrailPenalty", kTrailPenalty});
    out.push_back({w, "kBlockedPenalty", kBlockedPenalty});
    out.push_back({w, "kBlockedFade", kBlockedFade});
    out.push_back({w, "kStepNoise", kStepNoise});
    out.push_back({w, "kStuckNoise", kStuckNoise});
    out.push_back({w, "kStuckGain", kStuckGain});
    out.push_back({w, "kStuckRelief", kStuckRelief});
}

} // namespace goblins

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
#include "core/Strike.hpp"
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
// Здесь осталось то, что относится к животному и только к нему: сторонение
// чужого вида, пороги желаний, размножение, блуждание и охота.

// Стресса за объедание больше нет. Он был второй смертью поверх первой:
// куст и так теряет ровно ту биомассу, которую с него скусили, и стадо,
// пасущееся на месте, и без того держит луг на нуле развитости
// (kMinBiteGrowth ниже не пускает его глубже). Добивать куст ещё и
// смертью от условий значило считать одно и то же дважды — а платила за
// это трава, которая на выпасе не доживала до размера, с которого сеют,
// и участок оставался лысым навсегда. Теперь скушенный куст отрастает,
// как отрастал бы после засухи, и вымирания луга под стадом не случается.

// Болезни от тесноты здесь больше нет, и это стоит объяснить: она была
// третьей бедой здоровья и держала численность стада — теснее стоят свои,
// быстрее идёт зараза.
//
// Снята она не потому, что вредила, а потому, что делала ДВЕ вещи, и обе
// уже делаются иначе. Численность держит условие сытости: желание пары
// копится только у того, кому хватает еды и воды (adult && content ниже), —
// объело стадо луг, поднялся голод, и размножение встало раньше, чем пришла
// смерть. Ровно тот порядок, ради которого болезнь и заводилась, только без
// отдельного закона и одинаково для всех, у кого есть желания: у гоблина он
// тот же самый. Второе — рассеивание своих по местности — теперь не нужно
// вовсе: стадо должно стоять кучно (core/Walk.hpp, WalkHerd).
//
// Заодно ушла асимметрия, из-за которой этот закон был неказист: он
// касался одних травоядных. Хищник, гоблин и травоядное живут одним телом
// (core/Body.hpp), и беда, придуманная для одного из троих, в это тело не
// помещалась.

// Насколько травоядное сторонится чужого вида — того, кто ест ту же траву.
// Единственный оставшийся закон про "чужие рядом": своих больше ничто не
// разводит, а чужих разводит это.
//
// Это НЕ страх, и разница здесь несущая. Страх — желание (Flee), и чтобы
// победить, ему надо перебить и голод, и желание пары, а перебив — оставить
// зверя голодным и без потомства; на тесной карте два вида так заперли бы
// размножение друг другу. Сторонение живёт этажом ниже желаний — в выборе
// ноги (core/Walk.hpp, WalkShy): зверь не бросает еды и никуда не бежит, он
// лишь предпочитает ту сторону поляны, где чужого нет.
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

// Насколько охотящийся хищник сторонится ПОСТОРОННЕЙ добычи — той, за
// которой он сейчас не идёт.
//
// Цель он выбирает верно и заново каждый тик: берёт отбившегося, а не того,
// вокруг кого стоит полстада (kHuntCompany, core/Hunting.hpp). Но выбранная
// цель — это ещё не дорога к ней, а бьёт хищника всякий, у кого зубы в
// соседней клетке, охотятся на него или нет. Идя к одиночке напрямик через
// стадо, он собирал по дороге удары от тех, кого не трогал, и погибал, не
// дойдя.
//
// Сторонение, а не запрет и не страх, и это ровно тот случай, ради которого
// WalkShy написан: хищник не бросает охоты и никуда не сворачивает — он лишь
// заходит с той стороны, где посторонних нет. Вес больше травоядного (300):
// чужой вид у травоядного лишь ест ту же траву, а посторонняя добыча у
// хищника отнимает здоровье. Больше половины kAimPull ставить всё равно
// нельзя — тогда стадо начнёт уводить хищника от самой добычи.
constexpr int kBystanderShyness = 450;

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

// Голод, с которого выходят на охоту (kHuntHunger), осторожность к крупной
// добыче (kHuntCaution) и порог годного мяса (kMinBiteMeat) лежат в
// core/Hunting.hpp вместе с самим выбором добычи: этот закон спрашивает не
// только система, но и наблюдатель — он рисует найденную дорогу на карте.
// Досягаемость удара (kStrikeReach) переехала в core/Strike.hpp: дотягиваются
// теперь все, а не одни зубы.

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

// Намерение ударить: кто и кого. Ни силы, ни исхода здесь нет — их считает
// общий закон удара (core/Strike.hpp) при разрешении, из размера и меткости
// бьющего.
//
// Намерения собираются, а не исполняются на месте, по той же причине, что и
// доли в core/Share.hpp: двое, стоящие рядом, бьют друг друга в ОДИН тик, и
// исход не должен зависеть от того, кого EnTT хранит раньше. Урон при этом
// складывается, а не делится: двое, вцепившихся в одного, валят его вдвое
// быстрее.
//
// Ролей у сторон нет. Хищник бьёт добычу, за которой шёл; травоядное бьёт
// того хищника, от кого уже не убежать. Это одно и то же намерение и один и
// тот же закон, а не удар и ответ на него.
struct StrikeIntent {
    int target = 0;
    int striker = 0;
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
    std::vector<StrikeIntent> strikes;

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
    // Кто именно пугает — номер в снимке. Нужен затем, что бегущему мало
    // знать, куда бежать: если бежать уже поздно, он бьёт, и бьёт именно
    // того, от кого не ушёл (core/Strike.hpp).
    std::vector<int> threatOwner(animals.size(), -1);
    // Где ближайший чужой вид травоядных — считается в проходе 3 вместе со
    // страхом, а используется в проходе 4, при выборе шага (kRivalShyness).
    // Живёт здесь по той же причине, что и threatX/threatY: система не
    // хранит состояние между тиками, а животное не помнит того, кого больше
    // не видит.
    std::vector<int> rivalX(animals.size(), 0);
    std::vector<int> rivalY(animals.size(), 0);
    std::vector<bool> hasRival(animals.size(), false);
    // Где ближайший СВОЙ — тем же способом и для того же прохода, только с
    // обратным знаком: к своим тянет (core/Walk.hpp, WalkHerd). Считается
    // для всех, а не для одних травоядных: хищники друг другу тоже свои, и
    // ходить парами им ничто не мешает.
    std::vector<int> kinX(animals.size(), 0);
    std::vector<int> kinY(animals.size(), 0);
    std::vector<bool> hasKin(animals.size(), false);

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
    // Обратный указатель в снимок: страх называет клетку, а бить надо
    // конкретного зверя (см. threatOwner).
    std::vector<int> predatorOwner;
    for (std::size_t b = 0; b < animals.size(); ++b) {
        if (animals[b].predator) {
            predatorX.push_back(animals[b].x);
            predatorY.push_back(animals[b].y);
            predatorOwner.push_back(static_cast<int>(b));
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

        // Что время сделало с телом: расход на существование, взросление,
        // пищеварение, здоровье. Закон общий для всего живого и живёт в
        // core/Body.hpp — тело у гоблина то же самое.
        // Долголетие вида — свойство мира, а не генома: у травоядного и
        // хищника свои множители, и таблицы черт у них тоже свои.
        const int lifespan =
            animal.predator ? worldProperties.predatorLifespan : worldProperties.herbivoreLifespan;
        advanceBody(state, genome, lifespan, tick, animal.id);

        // Смерть от старости, от условий или от чужих зубов. Проверяется
        // здесь, а не внутри advanceBody, и это не мелочь: у тела могут быть
        // беды, отнимающие здоровье уже ПОСЛЕ общего закона (см. core/Body.hpp
        // — так было у болезни, так работает удар в п.8), и умереть от них
        // животное обязано в тот же тик, а не в следующий.
        //
        // Entity исчезает не сейчас, а при разрешении очереди команд
        // (05_Entity.md, п.5), и тело ложится падалью — одинаково, от чего
        // бы животное ни умерло.
        if (bodyDied(state, genome, lifespan)) {
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
                    threatOwner[a] = predatorOwner[b];
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
            // бы разогнать собственное стадо и остаться без пары. Своих,
            // наоборот, тянет друг к другу — но это уже не сторонение, а
            // отдельное слагаемое шага (core/Walk.hpp, WalkHerd).
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

        // Ближайший свой — тот, к кому тянет на ходу. Свой значит своего
        // вида и своей диеты: стадо собирается из тех, кто и есть стадо, а
        // не из всех, кто рядом пасётся.
        //
        // Считается для всех, включая хищников: закон один, и держаться
        // вместе не запрещено никому. Ближе kHerdKeep сородич не считается
        // вовсе — иначе стадо сошлось бы в одну клетку (см. WalkHerd).
        {
            const int sightCells = std::max(1, genome.perception);
            int kinDistance = 0;
            for (std::size_t b = 0; b < animals.size(); ++b) {
                if (b == a || !alive[b] || animals[b].predator != animal.predator ||
                    animals[b].genome->species != genome.species) {
                    continue;
                }
                const int dx = animals[b].x - animal.x;
                const int dy = animals[b].y - animal.y;
                const int distance = dx * dx + dy * dy;
                if (distance <= kHerdKeep * kHerdKeep || distance > sightCells * sightCells) {
                    continue;
                }
                if (hasKin[a] && distance >= kinDistance) {
                    continue;
                }
                kinX[a] = animals[b].x;
                kinY[a] = animals[b].y;
                hasKin[a] = true;
                kinDistance = distance;
            }
        }

        const bool adult = state.age >= maturityAgeOf(genome, lifespan) && state.growth >= kBreedingGrowth;
        // Готова ли мать заплатить за роды. Два условия, и оба — про цену, а
        // не про настроение: не отдыхает после прошлых родов и накопила
        // крупиц на целого детёныша (см. п.10, где они отдаются).
        //
        // Спрашивается здесь, до накопления желания, а не при встрече:
        // иначе пары сходились бы впустую и обнуляли желание друг другу, а
        // крупный вид, которому крупиц нужно втрое больше, не приносил бы
        // потомства вовсе, вечно встречаясь и вечно не рожая.
        //
        // Условие одно на оба пола: самец белка не отдаёт, но и он, пока не
        // дорос, ничего не сможет. Заводить разные правила для полов ради
        // этого значило бы описать один закон дважды.
        const bool canBearYoung = state.recovery == 0 && state.protein >= proteinNeedOf(genome);
        // Не бедствует: ни голод, ни жажда не дошли до предела, и нет
        // накопленного стресса — то есть в последние десятки тиков запасы
        // не кончались совсем.
        const bool content = state.health >= kFull && animal.hunger < kCalmNeed && animal.thirst < kCalmNeed;
        if (adult && content && canBearYoung) {
            // Желание пары — единственное, которого в теле не прочитать: оно
            // копится со временем у того, кому больше нечего хотеть.
            desire.mating = std::min(kFull, desire.mating + genome.breedingUrge);
        }
        desire.current = chooseDesire(animal, adult && content && canBearYoung);
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
        // Сколько своих стоит рядом с ней — столько защитников встретит
        // хищник, подойдя вплотную (kHuntCompany, core/Hunting.hpp).
        // Перебор по всем животным, как и у страха: их десятки, и обходится
        // он дешевле просмотра клеток вокруг.
        int company = 0;
        for (std::size_t c = 0; c < animals.size(); ++c) {
            if (c == b || !alive[c] || animals[c].predator) {
                continue;
            }
            if (std::abs(animals[c].x - animals[b].x) <= kCompanyRadius &&
                std::abs(animals[c].y - animals[b].y) <= kCompanyRadius) {
                ++company;
            }
        }
        preys.push_back(HuntPrey{animals[b].x, animals[b].y, animals[b].genome->speed,
                                  bodySize(*animals[b].state, *animals[b].genome), company,
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
        const int size = bodySize(state, genome);

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
        // Кого сторониться на ходу. У травоядного это ближайший чужой вид,
        // посчитанный ещё в п.3; у охотящегося хищника — ближайшая
        // ПОСТОРОННЯЯ добыча, и посчитать её раньше нельзя: пока цель не
        // выбрана, неизвестно, кто посторонний.
        WalkShy shy =
            hasRival[a] ? WalkShy{walkDirectionTo(animal.x, animal.y, rivalX[a], rivalY[a]), kRivalShyness}
                        : WalkShy{};

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
                        reachOf, Hunter{animal.x, animal.y, reach, effectiveSpeed, animal.hunger, size}, preys,
                        [&](int nx, int ny) { return carcassMeat[index(nx, ny)]; }, random);

                    // Добыча в пределах досягаемости зубов — бьём, и никуда
                    // при этом не идём.
                    if (choice.kind == HuntChoice::Kind::Prey && choice.atTeeth) {
                        strikes.push_back(
                            StrikeIntent{preyOwner[static_cast<std::size_t>(choice.prey)], static_cast<int>(a)});
                        busy = true;
                        break;
                    }

                    // Посторонняя добыча — та, за которой хищник сейчас НЕ
                    // идёт. Её он обходит стороной (kBystanderShyness): цель
                    // выбрана верно, а бьёт его всякий, у кого зубы в
                    // соседней клетке, охотятся на него или нет. Прежде он
                    // шёл к одиночке напрямик через стадо и собирал удары по
                    // дороге.
                    //
                    // Считается здесь, а не в п.3 вместе с прочими соседями:
                    // пока цель не выбрана, неизвестно, кто посторонний, — а
                    // цель хищник выбирает заново каждый тик, значит и
                    // сторониться ему каждый тик приходится других.
                    const int chosen = choice.kind == HuntChoice::Kind::Prey
                                            ? preyOwner[static_cast<std::size_t>(choice.prey)]
                                            : -1;
                    int bystanderDistance = 0;
                    for (std::size_t b = 0; b < animals.size(); ++b) {
                        if (static_cast<int>(b) == chosen || animals[b].predator || !alive[b]) {
                            continue;
                        }
                        const int dx = animals[b].x - animal.x;
                        const int dy = animals[b].y - animal.y;
                        const int distance = dx * dx + dy * dy;
                        if (distance > reach * reach) {
                            continue;
                        }
                        if (shy.direction >= 0 && distance >= bystanderDistance) {
                            continue;
                        }
                        shy = WalkShy{walkDirectionTo(animal.x, animal.y, animals[b].x, animals[b].y),
                                      kBystanderShyness};
                        bystanderDistance = distance;
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

                // Годна клетка или нет, решает не вся развитость травы, а
                // только та её часть, которую зубами берут (edibleGrowth,
                // core/Body.hpp): низ куртины на месте, а есть его нельзя,
                // и стоять над ним животному незачем.
                if (plantAt[here] != entt::null && edibleGrowth(plantGrowth[here]) > kMinBiteGrowth) {
                    bites.push_back(
                        ShareIntent{here, static_cast<int>(a), animal.id, genome.biteSize * size / kFull});
                    busy = true;
                } else {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return plantAt[cell] != entt::null &&
                                               edibleGrowth(plantGrowth[cell]) > kMinBiteGrowth && standable(nx, ny);
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
            case Desire::Flee: {
                // От опасности не идут "к цели" — от неё уходят, поэтому
                // направление шага считается ниже отдельно, а цели у
                // бегущего нет.
                //
                // А вот драться бегущий может: зубы в соседней клетке — и
                // это всё условие (core/Strike.hpp). Не ответ на укус и не
                // особое решение, а то же самое действие, каким хищник бьёт
                // добычу, просто повод другой.
                //
                // Проверки "а можно ли ещё убежать" здесь нет и не должно
                // быть. Она была — "бьёт, если преследователь не медленнее",
                // — и оказалась пустой: низ хищничьей полосы скорости выше
                // верхней середины травоядной (core/generation/
                // AnimalGenetics.hpp), поэтому "не уйти" было верно для
                // всех и всегда. Условие, которое никогда ничего не решает,
                // хуже отсутствия условия: его читают как закон.
                //
                // То, ради чего оно заводилось — чтобы хищника не забивало
                // всё стадо разом, — решается не здесь, а раньше: хищник
                // выбирает одиночку (kHuntCompany, core/Hunting.hpp). Не
                // тем, что стадо не бьёт, а тем, что в стадо не лезут.
                //
                // Бежать животное при этом не перестаёт: шаг ниже оно всё
                // равно сделает прочь. Удар — не занятие, а то, что выходит
                // само, когда зубы уже рядом.
                if (hasThreat[a] && threatOwner[a] >= 0 &&
                    strikeReaches(animal.x, animal.y, threatX[a], threatY[a])) {
                    strikes.push_back(StrikeIntent{threatOwner[a], static_cast<int>(a)});
                }
                break;
            }
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
        // Кого сторониться — посчитано выше: травоядному ближайший чужой
        // вид, охотящемуся хищнику ближайшая посторонняя добыча. Ни тот, ни
        // другой при этом ничего не бросает: сторонение лишь выбирает, с
        // какой стороны подойти (core/Walk.hpp, WalkShy).
        // К кому тянуться: к ближайшему своему, если тот дальше kHerdKeep
        // (п.3 выше). Ровно этим стадо и держится кучей — не строем и не
        // командой, а тем, что каждому чуть выгоднее шагнуть в сторону своих.
        const WalkHerd herd =
            hasKin[a] ? WalkHerd{walkDirectionTo(animal.x, animal.y, kinX[a], kinY[a]), kHerdPull} : WalkHerd{};
        // Утоптанность зверю пока ноль: топчут и притягиваются к тропам
        // сегодня только гоблины (docs/plan/10_Goblins_roadmap.md, шаг 4).
        // Закон при этом общий и написан для всех, кто ходит
        // (core/Trample.hpp), поэтому подключить сюда стадо — это заменить
        // ноль на tiles.trampled[index(nx, ny)] здесь и добавить trampleBy
        // в фазу шагов ниже. Две строки, и ни одной больше.
        const auto trodden = [](int, int) { return 0; };
        const WalkStep step =
            chooseStep(*animal.memory, animal.x, animal.y, aim, shy, herd, standable, trodden, random);

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

        // Делится не вся трава, а только съедобная её часть: сколько бы ртов
        // ни встало на клетку, низ куртины они не возьмут (edibleGrowth,
        // core/Body.hpp). Крупицы белка при этом по-прежнему считаются от
        // ПОЛНОЙ развитости — белок сидит во всём растении, и съевший
        // половину травы получает половину её крупиц, а не половину крупиц
        // съедобной части.
        const int edible = edibleGrowth(growthBefore);
        if (edible <= 0) {
            n = m;
            continue;
        }

        // Крупицы белка делятся между едоками целочисленно и без остатка:
        // каждому достаётся столько, сколько причитается на всё съеденное
        // им И теми, кто был до него, минус уже розданное. Накопителя доли
        // (proteinPending) при этом не нужно ни одному из них — куст
        // отдаёт свои крупицы здесь и сейчас, ровно по разу каждую.
        int eatenTotal = 0;
        int releasedTotal = 0;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(bites[k].want, edible, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *animals[static_cast<std::size_t>(bites[k].claimant)].state;
            const auto& genome = *animals[static_cast<std::size_t>(bites[k].claimant)].genome;

            feedBody(state, genome, eaten, kEnergyPerGrass);
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

            feedBody(state, genome, eaten, kEnergyPerBiomass);

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
            state.water = std::min(waterCapacityOf(genome), state.water + drinks[k].want);
        }
        n = m;
    }

    // --- 8. Удары ---
    // Все удары тика разрешаются разом, одним законом и без ролей: нет ни
    // нападающего, ни защищающегося, есть намерения, собранные в проходе 4
    // (StrikeIntent). Двое, стоящие рядом, бьют друг друга в этот же тик, и
    // ни один из двух не бьёт "в ответ".
    //
    // Разом — по той же причине, что и доли в core/Share.hpp: исход не
    // должен зависеть от того, кого EnTT хранит раньше. Бьют все живые на
    // начало прохода, поэтому убитый успевает ударить того, кто его убил, —
    // и это верно по сути: удары в одном тике одновременны.
    //
    // Урон складывается: двое на одном валят его вдвое быстрее. Смерть от
    // ран разрешается здесь же, а не на следующем тике, — иначе убитое
    // животное успело бы ещё раз пошевелиться. Каждый хоронится один раз,
    // сколько бы ударов в нём ни сошлось.
    for (const auto& strike : strikes) {
        const auto target = static_cast<std::size_t>(strike.target);
        const auto striker = static_cast<std::size_t>(strike.striker);
        if (!alive[target] || !alive[striker]) {
            continue;
        }
        const Animal& who = animals[striker];
        const Animal& whom = animals[target];
        const StrikeOutcome outcome =
            resolveStrike(bodySize(*who.state, *who.genome), bodySize(*whom.state, *whom.genome),
                          who.genome->hitChance, animalSeed, tick, who.id, whom.id);
        if (outcome.damage <= 0) {
            continue; // промах
        }
        animals[target].state->health -= outcome.damage;
        applyLameness(*animals[target].injury, outcome);
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
        calf.energy = std::min(givenEnergy, energyCapacityOf(childGenome));

        const int givenWater = mother.state->water * kBirthWaterShare / kFull;
        mother.state->water -= givenWater;
        calf.water = std::min(givenWater, waterCapacityOf(childGenome));

        // Белок детёнышу — ВЕСЬ, сколько нужно ему на полный рост, а не
        // доля материнского. Отсюда две вещи разом.
        //
        // Недорослей больше не бывает как явления. Прежде мать отдавала
        // треть своего, и телёнок крупного вида оставался мелким на всю
        // жизнь: расти ему было не из чего, а мелкий не может ни охотиться
        // (удар считается от размера), ни принести потомство. Теперь
        // потолок роста у новорождённого полный, и сдерживает его только
        // возраст.
        //
        // Роды дорожают ровно по величине вида: крупному нужно втрое больше
        // крупиц, чем мелкому, и копит он их втрое дольше. Отдельного
        // "штрафа за размер" для этого заводить не пришлось — цена и есть
        // само вещество тела.
        //
        // Платёжеспособность проверена ЗАРАНЕЕ, при накоплении желания (см.
        // "Желания" выше): мать без полного белка пары не хочет вовсе. Иначе
        // двое ходили бы встречаться впустую, обнуляя желание друг другу.
        const int givenProtein = std::min(mother.state->protein, proteinNeedOf(childGenome));
        mother.state->protein -= givenProtein;
        calf.protein = givenProtein;
        calf.growth = std::min(calf.growth, calf.protein * kFull / proteinNeedOf(childGenome));

        // Отдых матери — доля её собственной жизни (birthRestOf,
        // core/Body.hpp). Пока он не истёк, желание пары не копится вовсе.
        const int motherLifespan =
            mother.predator ? worldProperties.predatorLifespan : worldProperties.herbivoreLifespan;
        mother.state->recovery = birthRestOf(*mother.genome, motherLifespan);

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
    out.push_back({g, "kGrazeLeave", kGrazeLeave});
    out.push_back({g, "kEnergyPerGrass", kEnergyPerGrass});
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
    out.push_back({g, "kStrikeReach", static_cast<float>(kStrikeReach)});
    out.push_back({g, "kHuntCaution", static_cast<float>(kHuntCaution)});
    out.push_back({g, "kHuntPreyShare", kHuntPreyShare});
    out.push_back({g, "kHuntCompany", static_cast<float>(kHuntCompany)});
    out.push_back({g, "kCompanyRadius", static_cast<float>(kCompanyRadius)});
    out.push_back({g, "kRivalShyness", kRivalShyness});
    out.push_back({g, "kCarcassFearWeight", kCarcassFearWeight});
    out.push_back({g, "kStrikePerSize", kStrikePerSize});
    out.push_back({g, "kLameMaxTicks", static_cast<float>(kLameMaxTicks)});
    out.push_back({g, "kLameShare", kLameShare});
    out.push_back({g, "kMinLameShare", kMinLameShare});
    out.push_back({g, "kEnergyPerSize", static_cast<float>(kEnergyPerSize)});
    out.push_back({g, "kWaterPerSize", static_cast<float>(kWaterPerSize)});
    out.push_back({g, "kProteinPerSize", static_cast<float>(kProteinPerSize)});
    out.push_back({g, "kMaturityShare", static_cast<float>(kMaturityShare)});
    out.push_back({g, "kBirthRestShare", static_cast<float>(kBirthRestShare)});

    // Веса шага (core/Walk.hpp) — своей группой: они не про жизнь животного,
    // а про его походку, и подбираются вместе, друг против друга.
    constexpr const char* w = "Animals (walk)";
    out.push_back({w, "kTrailSteps", static_cast<float>(kTrailSteps)});
    out.push_back({w, "kAimPull", kAimPull});
    out.push_back({w, "kInertiaPull", kInertiaPull});
    out.push_back({w, "kTrailPenalty", kTrailPenalty});
    out.push_back({w, "kTroddenPull", kTroddenPull});
    out.push_back({w, "kBlockedPenalty", kBlockedPenalty});
    out.push_back({w, "kBlockedFade", kBlockedFade});
    out.push_back({w, "kStepNoise", kStepNoise});
    out.push_back({w, "kStuckNoise", kStuckNoise});
    out.push_back({w, "kStuckGain", kStuckGain});
    out.push_back({w, "kStuckRelief", kStuckRelief});
}

} // namespace goblins

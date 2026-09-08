#include "core/systems/GoblinSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/Body.hpp"
#include "core/Berries.hpp"
#include "core/Bonds.hpp"
#include "core/Character.hpp"
#include "core/Bound.hpp"
#include "core/Build.hpp"
#include "core/Carry.hpp"
#include "core/Carcass.hpp"
#include "core/Climb.hpp"
#include "core/Mind.hpp"
#include "core/Fatigue.hpp"
#include "core/Fear.hpp"
#include "core/Diagnostics.hpp"
#include "core/Hunting.hpp"
#include "core/Knowledge.hpp"
#include "core/Mating.hpp"
#include "core/Needs.hpp"
#include "core/Path.hpp"
#include "core/Random.hpp"
#include "core/Rest.hpp"
#include "core/Scale.hpp"
#include "core/Share.hpp"
#include "core/Strike.hpp"
#include "core/Talk.hpp"
#include "core/Resources.hpp"
#include "core/Store.hpp"
#include "core/Work.hpp"
#include "core/TileSnapshot.hpp"
#include "core/Trample.hpp"
#include "core/Walk.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/BerryComponent.hpp"
#include "core/components/BondsComponent.hpp"
#include "core/components/BuildingComponent.hpp"
#include "core/components/CarriedComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/CharacterComponent.hpp"
#include "core/components/FatigueComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/GoblinDesireComponent.hpp"
#include "core/components/GoblinTribesComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/InjuryComponent.hpp"
#include "core/components/KnowledgeComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/PredatorComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/StoreComponent.hpp"
#include "core/components/SiteComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/GoblinCharacters.hpp"
#include "core/generation/GoblinGenetics.hpp"

namespace goblins {

namespace {

// Пороги желаний. Числа те же, что у животных, но константы свои, и это не
// оплошность: у гоблина скоро появятся желания, которых у зверя нет и быть
// не может (отдых, ноша, работа), и равновесие между ними придётся крутить
// отдельно. Общим здесь остаётся разум (core/Mind.hpp), а не значения,
// которыми он настроен.
constexpr int kDesireFloor = 350;
constexpr int kDesireSwitch = 150;

// С какого страха гоблин бросает всё разом, не спрашивая, чем он занят.
//
// Порог этот — не третье число в ряду к порогу и инерции, а признание того,
// что страх и голод меряются одной шкалой, но значат разное. Голод 800 и
// страх 800 — это "я не ел полдня" и "зубы в трёх шагах"; складывать их в
// одном сравнении можно ровно до тех пор, пока зубы далеко.
//
// Замер, ради которого порог и заведён (3000 тиков, поголовье около
// полусотни): при хищнике в СОСЕДНЕЙ клетке бежали 61% гоблинов, 29%
// продолжали есть, 7% — искать пару. Числа сходятся точно: страх у самых
// зубов доходит до 870 (seenScare при зоркости 10), а победить ему надо не
// порог, а голод плюс инерцию, — то есть голодному за 720 не страшно ничто.
// Ошибка была не в страхе, а в том, что зубы стояли в очереди наравне с
// обедом.
//
// Семьсот — это зубы ближе половины поля зрения (seenScare, core/Fear.hpp:
// на краю видимости страх равен порогу, у самых зубов полный). Ниже ставить
// нельзя: хищник, бродящий по краю округи, ввёл бы поселение в вечное
// бегство, и гоблины перестали бы есть. Выше — почти бесполезно: гоблин
// медленнее всех в этом мире (300..900 против 900..1800), и вся его защита в
// том, чтобы тронуться РАНЬШЕ, — добежать до своих, влезть на камень, а не
// перегнать зубы.
//
// Само правило при этом общее (Option::panic, core/Mind.hpp) и у зверя такое
// же: разойдись оно по видам — и вышло бы, что зубы кусают по-разному в
// зависимости от того, кого кусают (core/Fear.hpp). Своё здесь только
// число, и своё оно ровно по той же причине, по какой свои порог и инерция.
constexpr int kPanic = 700;

// Продолжение рода. Желание копится только у взрослого, доросшего и не
// бедствующего (kCalmNeed — предел голода и жажды, при котором ещё не
// бедствие), а идти искать пару гоблин начинает с kMateDesire.
constexpr int kBreedingGrowth = 900;
constexpr int kCalmNeed = 750;
constexpr int kMateDesire = 600;

// С какой твёрдостью ложится в память поляна, на которой видели ровню
// (PlaceKind::Mate).
//
// Своё число, а не kRememberGain, и ровно по той же причине, по какой своё
// число есть у испуга (kScareMark, core/Fear.hpp): remember занимает чужой
// слот только тем, что твёрже занятого (core/Knowledge.hpp), а у
// обжившегося гоблина все восемь слотов держат сотни. Прибавка ценой в сорок
// падала бы на пол каждый раз — и незнакомую женщину гоблин не запоминал бы
// НИКОГДА, сколько бы раз её ни видел. Заметить это было нечем: место просто
// не появлялось, а ищущий пару уходил блуждать, будто рядом никого и нет.
//
// Триста — то же, что у испуга, и по тем же двум меркам: против забывания
// (kForgetRate = 1) одна встреча тает за триста тиков, а против порога
// желаний (350) одного взгляда на дело не хватает. Место, где увидел
// кого-то однажды, — случайность; место, где ходят, — свойство места, и
// подтверждается оно каждым тиком, пока ровня в виду.
constexpr int kMetMark = 300;

// Насколько симпатия укрепляет ту же память сверх этого.
//
// Двести сверху означают, что поляна, где ходят СВОИ, держится в голове
// вдвое дольше поляны, где ходят чужие, и вытесняет из неё больше чужого.
// Знакомство при этом не условие, а прибавка: незнакомую женщину гоблин
// запомнит тоже — знакомство лишь решает, какое из мест переживёт другое.
constexpr int kMateMark = 200;

// Усталость и отдых числами не описываются здесь вовсе: закон общий для
// всех, кто ходит, и живёт в core/Fatigue.hpp. Гоблинского в нём ровно
// одно — то, что гоблин ложится не где стоит, а на годном месте
// (core/Rest.hpp).

// Сколько ягод срывает за тик взрослый гоблин (у мелкого — доля от размера
// тела, как и укус). Три штуки: полный куст (kBerryMax = 12,
// core/Berries.hpp) обирается за четыре тика — быстро, потому что рвут
// руками, а не жуют.
//
// Из этого и берётся весь смысл ягодника: обирается он за считанные тики, а
// наливается обратно тысячами. Значит, наевшийся уходит, а вернуться сюда
// имеет смысл не раньше, чем куст успеет завязать новые, — и между уходом и
// возвращением как раз и лежит всё остальное: вода, отдых, тропа.
constexpr int kBerryPick = 3;

// Насколько сильно гоблина гонит запасать. Число постоянное, а не растущее
// от чего-либо, и лежит оно между порогом желаний (kDesireFloor) и голодом:
// **запасается тот, кого больше ничто не гонит**.
//
// Постоянным оно и должно быть. Голод растёт от пустого желудка, усталость —
// от пройденного пути, а запасать хочется ровно тогда, когда есть силы и
// время; сделать эту срочность растущей значило бы завести гоблину тревогу о
// будущем, которой у него нет и которой закон мира не требует.
//
// Это первая работа в мире: труд, который не кормит сейчас. На шаге
// "постройки" из него вырастет настоящая.
constexpr int kHaulUrge = 400;

// Насколько сильно гоблина гонит достраивать начатое. Ровно как у запаса,
// число постоянное и лежит между порогом желаний и голодом: строит тот, кого
// больше ничто не гонит.
//
// Само НАЧАЛО стройки этим числом не меряется — его меряет нехватка (см.
// buildLack): пока место не станет плохим, строить незачем. А вот начатое
// надо доводить до конца, и держит гоблина у площадки уже не нехватка, а сам
// незаконченный замысел.
constexpr int kBuildUrge = 400;

// С какой вероятностью ничего не желающий гоблин всё-таки делает шаг.
// Постоянно бродящий выглядит нервным и зря жжёт энергию, полностью
// неподвижный — мёртвым.
constexpr int kWanderChance = 250;

// Сколько тиков гоблин держит одно направление поиска, когда желаемого не
// видно. Единственный способ найти что-то за пределами своей видимости:
// случайный шаг в случайную сторону уводит от исходной точки как корень из
// числа шагов, а прямая ходьба — линейно.
//
// Со шага "память места" этот способ станет запасным: у гоблина появится
// куда возвращаться, и блуждание останется тем, чем оно и является, —
// способом узнать новое, а не способом дойти до известного.
constexpr std::uint64_t kRoamTicks = 40;

// Насколько далеко от дома гоблин отпускает себя блуждать.
//
// Блуждание — способ узнать НОВОЕ: направление берётся жребием и держится
// сорок тиков (kRoamTicks), потому что случайный шаг уводит от исходной точки
// как корень из числа шагов, а прямая ходьба — линейно. Ровно поэтому
// блуждание и не возвращает: оно и не должно.
//
// Возвращать должен дом, и до этого числа возвращать его было нечему. Домой
// гоблина тянуло ровно две вещи — усталость и полные руки, — а любое желание,
// не нашедшее цели (голодный без памяти о еде, ищущий пару, строитель без
// площадки), уходило по прямой и не приходило назад. Тяга к своим (kHerdPull,
// core/Walk.hpp) от этого не спасает: она действует, только пока своих ВИДНО,
// а ушедшего из виду не возвращает ничто.
//
// Двадцать — это две-четыре зоркости (5..18): своя округа, в которой лежат
// ягодник, водопой и лагерь. Внутри неё гоблин блуждает как блуждал, за ней
// жребий заменяется направлением на дом — то есть у обжившегося блуждание
// становится блужданием ПО ОКРУГЕ, а не по карте. Больше — и привязь
// перестаёт держать; меньше — и гоблин не найдёт нового ягодника, когда
// старый объеден.
//
// Дальнего предела здесь не нужно, он уже есть: recall отсекает место, у
// которого твёрдость минус расстояние ушла в минус (core/Knowledge.hpp), —
// ушедший за две с половиной сотни шагов дома просто не помнит, и тянуть его
// некуда.
constexpr int kHomeRange = 20;

// --- Намерения ---
// Собираются при обходе гоблинов и исполняются после него: на один куст,
// одну тушу и один водопой могут прийти сразу несколько, и решать спор
// порядком обхода Entity нельзя (04_WorldModel.md, п.8). Дележ — общий
// закон (core/Share.hpp, ShareIntent).

struct StepIntent {
    int goblin = 0;
    int x = 0;
    int y = 0;
};

struct MateIntent {
    std::size_t cell = 0;
    int goblin = 0;
    std::uint64_t id = 0;
    int tribe = 0;
    Sex sex = Sex::Female;
};

// Кто к кому подошёл поговорить. Второго здесь, в отличие от пары, называют
// сразу: пару ждут на клетке, и на одной клетке их может собраться трое, а
// разговор начинают с КЕМ-ТО — заговоривший уже выбрал, к кому шёл
// (chooseCompanion, core/Talk.hpp), и решать за него потом было бы подменой.
struct TalkIntent {
    int speaker = 0;
    std::uint64_t speakerId = 0;
    std::uint64_t listenerId = 0;
};

// Кто по кому отмахнулся. Собирается намерением, как и всё прочее, и по той
// же причине: по одному зверю могут отмахнуться сразу несколько, и урон обязан
// сложиться, а не достаться тому, кого EnTT хранит раньше
// (02_CorePrinciples.md, п.12a).
//
// Ролей у сторон нет и здесь: это то же самое действие, каким хищник бьёт
// добычу (core/Strike.hpp), просто повод другой. Ответным ударом оно не
// называется — ответ подразумевал бы очередь, а удары одного тика
// одновременны.
struct BlowIntent {
    int goblin = 0;
    int beast = 0;
};

// Живой гоблин в снимке этого тика. Указатели на компоненты держать
// безопасно: за время обхода систем структура хранилища не меняется —
// создание и удаление Entity идёт только через очередь команд
// (05_Entity.md, п.5).
struct Goblin {
    entt::entity entity = entt::null;
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    AnimalComponent* state = nullptr;
    const AnimalGenomeComponent* genome = nullptr;
    GoblinDesireComponent* desire = nullptr;
    MovementComponent* memory = nullptr;
    // Усталость. Общее для всего живого (FatigueComponent, core/Fatigue.hpp):
    // устают все, кто ходит, и гоблинского здесь только место, на котором он
    // ложится.
    FatigueComponent* tired = nullptr;
    // Память мест. Единственное, чего у зверя нет вовсе.
    KnowledgeComponent* mind = nullptr;
    // Руки. Общее для всего живого (core/Carry.hpp), просто носит пока
    // только гоблин.
    CarriedComponent* hands = nullptr;
    // Нрав и склонности. За жизнь не меняются, поэтому указатель константный:
    // случайная правка нрава посреди тика была бы неотличима от закона.
    const CharacterComponent* nature = nullptr;
    // Знакомые. Меняются каждый разговор — значит, не константа.
    BondsComponent* bonds = nullptr;
    // Увечье. Общее для всего живого (core/Strike.hpp): хромает всякий, кого
    // покусали, и гоблинского здесь нет ничего.
    InjuryComponent* injury = nullptr;

    // Голод, жажда и страх живут здесь, в снимке тика, а не в компоненте: все
    // три пересчитываются заново каждый тик — первые два из тела
    // (core/Needs.hpp), третий из того, кто стоит рядом (core/Fear.hpp), — и
    // пережить тик им незачем.
    int hunger = 0;
    int thirst = 0;
    int fear = 0;
};

// Позыв к работе, каким он стал после нрава: УРОВЕНЬ даёт трудолюбие, а
// НАПРАВЛЕНИЕ — склонность к теме этого дела (core/Character.hpp).
//
// Один помощник на оба дела, а не два числа в двух местах: запас и стройка
// различаются только основанием и темой, и разойтись этим двум поправкам
// было бы не на чем, кроме опечатки.
//
// Поправка кладётся на ПОСТОЯННЫЙ позыв, а не на нехватку. Нехватка — это
// то, чего в мире недостаёт на самом деле (дырявый навес, куча под дождём);
// умножь её на нрав, и трудолюбивый видел бы дыру там, где её нет, а ленивый
// не видел бы настоящей.
int workUrgeOf(const CharacterComponent& nature, int base, Topic topic) {
    return interestUrge(workUrge(base, nature.diligent), interestIn(nature, topic));
}

// Насколько открыт край обжитого вокруг этой клетки (core/Bound.hpp) — и в
// какую сторону он открыт сильнее всего.
//
// Считается по восьми соседям и только по ним: гоблин чувствует край там, где
// стоит, а не вычисляет контур лагеря по карте. Кольцо получается из того,
// что так делает каждый на своём краю (02_CorePrinciples.md, п.14).
//
// towardX/towardY — куда воткнуть кол: открытый внешний подход, а из
// нескольких — ближайший к вспомненной опасности. Ближе, потому что
// огораживаются ОТ ЧЕГО-ТО, а не вообще.
struct Rim {
    int openness = 0;
    int towardX = 0;
    int towardY = 0;
    bool found = false;
};

// Куда именно воткнуть кол, решает РАЗУМ (core/Mind.hpp): мир складывает
// открытые подходы и вес каждого, а вес тем больше, чем ближе подход к
// вспомненной опасности. Огораживаются ОТ ЧЕГО-ТО, а не вообще.
//
// Открытость же считает сам мир и разуму не отдаёт: это не выбор, а факт —
// какая доля подходов не закрыта (core/Bound.hpp).
template <typename Outside, typename Shut>
Rim rimAround(int x, int y, int dangerX, int dangerY, Mind mind, std::uint64_t& random, Outside&& outside,
              Shut&& shut) {
    Rim rim{0, x, y, false};
    int outsideCount = 0;
    int shutCount = 0;
    Option ways[8];
    int wayCount = 0;
    for (int dir = 0; dir < 8; ++dir) {
        const int nx = x + kWalkX[dir];
        const int ny = y + kWalkY[dir];
        if (!outside(nx, ny)) {
            continue;
        }
        ++outsideCount;
        if (shut(nx, ny)) {
            ++shutCount;
            continue;
        }
        // Дальше опасности некуда: восемь соседей, значит квадрат расстояния
        // до неё не больше... чего угодно. Поэтому вес считается вычитанием
        // из заведомо большего, а не отрицанием: отрицательных весов разум не
        // понимает, и понимать не должен — вес это "насколько хорошо".
        const int away = (nx - dangerX) * (nx - dangerX) + (ny - dangerY) * (ny - dangerY);
        ways[wayCount++] = Option{dir, nx, ny, std::max(1, kFull - away), 0, false, 0};
    }
    rim.openness = opennessOf(outsideCount, shutCount);
    if (wayCount == 0) {
        return rim;
    }
    const Choice choice = decide(mind, std::span<const Option>(ways, wayCount), Temper{}, random);
    if (choice.made) {
        rim.found = true;
        rim.towardX = choice.x;
        rim.towardY = choice.y;
    }
    return rim;
}

// Какое желание сейчас гонит гоблина.
//
// САМ ВЫБОР ГОБЛИНУ НЕ ПРИНАДЛЕЖИТ. Его делает разум (core/Mind.hpp), и разум
// сменный: порог, инерция, паника и жребий — его устройство, а не закон мира
// (02_CorePrinciples.md, п.6). Здесь остаётся только гоблинское: чего он
// может хотеть, чем меряется срочность каждого желания и какими числами
// настроен его разум.
//
// Страх СТАРШЕ ВСЕХ (Option::rank) и потому побеждает при равенстве — ровно
// как у зверя, и по той же причине: сытость подождёт, зубы — нет. Место под
// него было оставлено заранее, когда хищник гоблина ещё не видел; теперь
// видит, и желание встало туда, где ему и назначено.
//
// Старшинство названо числом, а не местом в списке: тот же разум выбирает и
// клетку, где у перебора порядок ничего не значит.
GoblinDesire chooseGoblinDesire(const Goblin& goblin, bool readyToMate, bool hasHome, int building,
                                bool companionNear, Mind mind, std::uint64_t& random) {
    const GoblinDesireComponent& desire = *goblin.desire;
    const CharacterComponent& nature = *goblin.nature;
    const int mating = readyToMate && desire.mating >= kMateDesire ? desire.mating : 0;
    // Запасать некуда — незачем и начинать. Гейт стоит здесь, а не в самой
    // ветке: желание, которое нельзя исполнить, не должно даже побеждать
    // (иначе гоблин "занят" тем, чего не делает).
    //
    // Само же число больше не общее на всех: ленивый проваливается ниже
    // порога желаний и за ношу не берётся вовсе, трудолюбивый бросает её
    // только ради еды и воды (core/Character.hpp). Тема запаса — еда: за ней
    // и ходят с пустыми руками.
    const int hauling = hasHome ? workUrgeOf(nature, kHaulUrge, Topic::Food) : 0;
    // Поговорить не с кем — не о чем и тосковать. Тот же гейт и по той же
    // причине, что у запаса, и здесь он даже нужнее: одинокий гоблин иначе
    // накопил бы полное желание, застрял бы в нём навсегда (инерция!) и
    // перестал бы и есть, и работать.
    const int talking = companionNear ? std::clamp(desire.talking, 0, kFull) : 0;

    // Порядок — приоритет при равенстве, побеждает последний. Отдых стоит
    // почти первым и потому проигрывает всему, кроме разговора: усталость
    // никого не убивает, а голод и жажда убивают. Лечь гоблин должен тогда,
    // когда его больше ничто не гонит, — и это не поблажка, а точное
    // описание того, чем отдых отличается от еды. Ниже него — только
    // разговор: без него можно прожить и вовсе.
    //
    // Отдельного "чем занят сейчас" здесь больше нет: занятие помечается
    // прямо в варианте (Option::current). Прежде рядом со списком стоял
    // второй switch, повторявший те же восемь величин, и держать два списка
    // в согласии приходилось руками — разъехались бы они молча.
    const auto busy = [&](GoblinDesire kind) { return desire.current == kind; };
    const Option options[] = {
        // Разговор — ПЕРВЫМ, то есть проигрывает при равенстве всем, включая
        // отдых. Так и задумано: болтают тогда, когда не гонит вообще ничто,
        // — это единственное занятие в списке, без которого можно прожить.
        {static_cast<int>(GoblinDesire::Talk), 0, 0, talking, 0, busy(GoblinDesire::Talk), 0},
        {static_cast<int>(GoblinDesire::Rest), 0, 0, goblin.tired->fatigue, 1, busy(GoblinDesire::Rest), 0},
        // Запасание — сразу после отдыха и раньше всего остального в списке,
        // то есть проигрывает и голоду, и жажде, и паре: набирать впрок имеет
        // смысл только сытым.
        {static_cast<int>(GoblinDesire::Haul), 0, 0, hauling, 2, busy(GoblinDesire::Haul), 0},
        // Стройка — после запаса, то есть при равенстве побеждает она: запас
        // делается впрок и подождёт, а стройка — ответ на конкретную нехватку
        // здесь и сейчас. Голоду и жажде она всё равно проигрывает.
        {static_cast<int>(GoblinDesire::Build), 0, 0, building, 3, busy(GoblinDesire::Build), 0},
        {static_cast<int>(GoblinDesire::Food), 0, 0, goblin.hunger, 4, busy(GoblinDesire::Food), 0},
        {static_cast<int>(GoblinDesire::Water), 0, 0, goblin.thirst, 5, busy(GoblinDesire::Water), 0},
        {static_cast<int>(GoblinDesire::Mate), 0, 0, mating, 6, busy(GoblinDesire::Mate), 0},
        // Зубы — последними: при равенстве побеждают всё, включая голод и
        // пару. Голодный доживёт до следующего куста, съеденный — нет. А выше
        // kPanic они и вовсе не обсуждаются (Option::panic): инерция придумана
        // против метаний между едой и водой, которые никуда не денутся; зубы
        // денутся, и ждать они не станут.
        {static_cast<int>(GoblinDesire::Flee), 0, 0, goblin.fear, 7, busy(GoblinDesire::Flee), kPanic},
    };

    const Choice choice = decide(mind, options, Temper{kDesireFloor, kDesireSwitch}, random);
    return choice.made ? static_cast<GoblinDesire>(choice.tag) : GoblinDesire::Idle;
}

} // namespace

void GoblinSystem(World& world, CommandQueue& commands) {
    const int width = world.area().width();
    const int height = world.area().height();
    const std::size_t cellCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (cellCount == 0) {
        return;
    }

    auto index = [width](int x, int y) { return static_cast<std::size_t>(y) * width + x; };

    auto& registry = world.registry();
    const auto& worldProperties = registry.get<const WorldPropertiesComponent>(world.worldEntity());
    // Каким разумом думают существа этого мира (core/Mind.hpp). Одно значение
    // на весь тик и на всех: разум — свойство мира, а не особи, и меняется он
    // только регенерацией.
    const Mind mind = worldProperties.toggles.lotteryMind ? Mind::Lottery : Mind::Greedy;
    // Мутация в тысячных долях вложения (core/Scale.hpp) — сам расклад
    // бюджета дробный, это генерация, а не состояние мира.
    const float mutationRate = static_cast<float>(worldProperties.goblinMutationRate) / kFull;
    const auto goblinSeed = static_cast<std::uint64_t>(worldProperties.goblinRandomSeed);
    const std::uint64_t tick = registry.get<const TimeComponent>(world.worldEntity()).tick;
    const auto& tribes = registry.get<const GoblinTribesComponent>(world.worldEntity());

    // --- 1. Снимок гоблинов ---
    // Разреженно, а не плотным массивом на всю Область: гоблинов десятки, а
    // клеток десятки тысяч.
    std::vector<Goblin> goblins;
    // Перечень компонентов здесь — жёсткое И: гоблин, у которого нет хотя бы
    // одного, не сломается с шумом, а молча выпадет из мира и замрёт навсегда.
    // Поэтому нрав и знакомых обязаны положить ВСЕ трое, кто заводит гоблина:
    // расселение (GoblinSeeding), рождение (п.9 ниже) и чтение файла мира
    // (server/WorldSave.cpp).
    auto goblinView =
        registry.view<AnimalComponent, AnimalGenomeComponent, GoblinDesireComponent, IdentityComponent,
                       MovementComponent, PositionComponent, GoblinComponent, FatigueComponent,
                       KnowledgeComponent, CarriedComponent, CharacterComponent, BondsComponent,
                       InjuryComponent>();
    for (const auto entity : goblinView) {
        const auto& position = goblinView.get<PositionComponent>(entity);
        if (!world.area().inBounds(position.x, position.y)) {
            continue;
        }
        goblins.push_back(Goblin{entity, goblinView.get<IdentityComponent>(entity).id, position.x, position.y,
                                  &goblinView.get<AnimalComponent>(entity),
                                  &goblinView.get<AnimalGenomeComponent>(entity),
                                  &goblinView.get<GoblinDesireComponent>(entity),
                                  &goblinView.get<MovementComponent>(entity),
                                  &goblinView.get<FatigueComponent>(entity),
                                  &goblinView.get<KnowledgeComponent>(entity),
                                  &goblinView.get<CarriedComponent>(entity),
                                  &goblinView.get<CharacterComponent>(entity),
                                  &goblinView.get<BondsComponent>(entity),
                                  &goblinView.get<InjuryComponent>(entity)});
    }
    // Гоблинов нет — делать системе нечего. В отличие от AnimalSystem, за
    // которой числится ещё и гниение падали, у этой своих обязанностей перед
    // миром нет ни одной: она только про гоблинов.
    if (goblins.empty()) {
        return;
    }

    // --- 2. Снимок тайлов ---
    // Свой, а не общий с AnimalSystem, и снимается сейчас, а не в начале
    // тика: стадо уже поело, и гоблин обязан видеть то, что от куста
    // осталось (05_Entity.md, п.6 — системы разговаривают состоянием
    // компонентов).
    TileSnapshot tiles;
    tiles.capture(world);
    const std::vector<entt::entity>& terrain = tiles.terrain;
    const std::vector<int>& waterAt = tiles.waterAt;
    const std::vector<entt::entity>& plantAt = tiles.plantAt;
    const std::vector<int>& plantGrowth = tiles.plantGrowth;
    const std::vector<entt::entity>& bushAt = tiles.bushAt;
    const std::vector<int>& berriesAt = tiles.berriesAt;
    const std::vector<int>& storeFood = tiles.storeFood;
    const std::vector<int>& storeMaterial = tiles.storeMaterial;
    const std::vector<int>& storeTotal = tiles.storeTotal;
    const std::vector<int>& canopyAt = tiles.canopy;
    const std::vector<int>& beddingAt = tiles.bedding;
    const std::vector<BuildKind>& siteKind = tiles.siteKind;
    const std::vector<int>& carcassMeat = tiles.carcassMeat;

    std::vector<ShareIntent> bites;  // трава
    std::vector<ShareIntent> picks;  // ягоды
    std::vector<ShareIntent> scoops; // куча
    std::vector<ShareIntent> harvests; // трава и ветки на материал
    std::vector<StepIntent> works;   // единицы труда, вложенные в площадки
    std::vector<ShareIntent> meals;  // падаль
    std::vector<ShareIntent> drinks;
    std::vector<StepIntent> steps;
    std::vector<MateIntent> matings;
    std::vector<TalkIntent> talks;
    std::vector<BlowIntent> blows;   // кто отмахнулся от зубов
    // Варианты для разума (core/Mind.hpp). Живут снаружи проходов и
    // переиспользуются, как и волна дороги: видимая округа зоркого гоблина —
    // тысяча с лишним клеток, и складывать её заново каждый раз значило бы
    // выделять память на каждое решение.
    std::vector<Option> sights;

    // Край обжитого вокруг клетки — по снимку этого тика. Живёт здесь, а не
    // внутри прохода: спрашивают его дважды — п.3 (насколько гонит городиться)
    // и п.4 (куда воткнуть кол), — и два одинаковых ответа обязаны быть одним.
    //
    // Закрытость подхода решается ЗАКОНОМ проходимости для ЗВЕРЯ (kOnLegs):
    // подход закрыт ровно тогда, когда зверю там не встать, — водой, валуном,
    // краем мира или уже стоящим забором. Спрашивать здесь годность для
    // гоблина было бы враньём: он-то пройдёт везде, и открытых сторон у него
    // не оказалось бы вовсе.
    // Жребий края — свой поток, как и у выбора занятия: два розыгрыша одного
    // гоблина в один тик, начатые с одного состояния, дали бы одно и то же
    // число.
    std::uint64_t rimRandom = 0;
    const auto rimHere = [&](int x, int y, int dangerX, int dangerY) {
        rimRandom = mixSeed(mixSeed(tick, goblinSeed), static_cast<std::uint64_t>(index(x, y)));
        const std::size_t at = index(x, y);
        return rimAround(
            x, y, dangerX, dangerY, mind, rimRandom,
            [&](int nx, int ny) {
                return world.area().inBounds(nx, ny) &&
                       outsideOf(tiles.trampled[at], tiles.trampled[index(nx, ny)]);
            },
            [&](int nx, int ny) {
                const std::size_t cell = index(nx, ny);
                const bool beastStands =
                    standableAt(world.area().isBlocked(nx, ny), tiles.terrain[cell] != entt::null,
                                tiles.waterAt[cell], tiles.terrainHeight[cell], tiles.fenceAt[cell], kOnLegs);
                return approachShut(beastStands, tiles.fenceAt[cell]);
            });
    };

    // Кто рядом стоит — тем, что о нём видно со стороны (core/Talk.hpp).
    // Список собирается ДО желаний, а не после, как пары: желание поговорить
    // само зависит от того, есть ли рядом живая душа, и спросить об этом надо
    // раньше, чем оно посчитано.
    //
    // "Жив" здесь пока значит "стоял живым в начале тика" — умереть от
    // истощения гоблин может только в п.3, ниже. Для вопроса "есть ли кому
    // сказать слово" этого довольно: тело на соседней клетке видно и тому, кто
    // не знает, что оно последний тик доживает. А вот выбирать собеседника
    // (п.4) полагается уже среди живых, и потому список правится между п.3 и
    // п.4.
    std::vector<Companion> companions;
    companions.reserve(goblins.size());
    for (const auto& goblin : goblins) {
        companions.push_back(Companion{goblin.id, goblin.x, goblin.y, goblin.genome->species, true});
    }

    // Кто в этом мире с зубами. Список свой, а не общий с AnimalSystem: снимки
    // у систем разные по замыслу (см. п.2) — гоблин видит мир, по которому
    // стадо уже прошло, и хищника он тоже должен видеть там, где тот стоит
    // сейчас, а не там, где стоял до своего шага.
    //
    // Собирается один раз на тик, а не заново для каждого гоблина: хищников
    // десятки, гоблинов тоже, и произведение считать незачем.
    //
    // Указатели на тело и увечье лежат здесь же — не ради страха, а ради
    // сдачи: испуганный гоблин бьёт (см. п.7b), а бить надо конкретного
    // зверя. Генома среди них нет: исход удара считается по размерам и по
    // меткости БЬЮЩЕГО (core/Strike.hpp), а бьёт здесь гоблин.
    struct Beast {
        std::uint64_t id = 0;
        int x = 0;
        int y = 0;
        int size = 0;
        AnimalComponent* state = nullptr;
        InjuryComponent* injury = nullptr;
    };
    std::vector<Beast> beasts;
    {
        auto beastView = registry.view<AnimalComponent, AnimalGenomeComponent, IdentityComponent,
                                        InjuryComponent, PositionComponent, PredatorComponent>();
        for (const auto entity : beastView) {
            const auto& position = beastView.get<PositionComponent>(entity);
            if (!world.area().inBounds(position.x, position.y)) {
                continue;
            }
            auto& body = beastView.get<AnimalComponent>(entity);
            const auto& beastGenome = beastView.get<AnimalGenomeComponent>(entity);
            beasts.push_back(Beast{beastView.get<IdentityComponent>(entity).id, position.x, position.y,
                                    bodySize(body, beastGenome), &body,
                                    &beastView.get<InjuryComponent>(entity)});
        }
    }

    // --- 3. Тело и желания ---
    // Отдельным проходом от решений (п.4) намеренно: гоблин, выбирая пару,
    // смотрит, чего хочет сосед, — и если бы желания и решения считались в
    // одном проходе, сосед, которого EnTT хранит позже, был бы ещё с
    // прошлотиковым желанием. Порядок в памяти не может быть причиной
    // события в мире (02_CorePrinciples.md, п.12a).
    std::vector<bool> alive(goblins.size(), true);
    // Откуда исходит опасность — считается здесь, в п.3, и используется в
    // п.4 (куда бежать) и в п.7b (кого бить). Живёт снаружи компонентов: страх
    // пересчитывается каждый тик заново, а гоблин не помнит зверя, которого
    // больше не видит. Помнит он только МЕСТО (PlaceKind::Danger), и это
    // разные вещи: зверь ушёл, место осталось.
    std::vector<int> threatX(goblins.size(), 0);
    std::vector<int> threatY(goblins.size(), 0);
    std::vector<bool> hasThreat(goblins.size(), false);
    // Номер в списке зубов, а не клетка: страх называет клетку, а бить надо
    // конкретного зверя.
    std::vector<int> threatBeast(goblins.size(), -1);
    // Кто вырос. Считается в п.3, а нужно в п.4 — и не самому гоблину, а
    // тем, кто на него смотрит: ровню (mateKind, core/Mating.hpp) от ребёнка
    // отличают со стороны.
    std::vector<char> grown(goblins.size(), 0);
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        Goblin& goblin = goblins[g];
        auto& state = *goblin.state;
        const auto& genome = *goblin.genome;
        auto& desire = *goblin.desire;

        advanceBody(state, genome, worldProperties.goblinPace, tick, goblin.id);

        // Болезни от тесноты у гоблина нет (поселение тесно по сути), а вот
        // зубы теперь есть: бьёт его AnimalSystem, которая идёт раньше, и
        // здоровье в теле к этому мгновению уже убавлено. Хоронит его эта
        // система, и только она: тот же вопрос обеим системам отвечает
        // одинаково, и хорони обе — туша легла бы дважды.
        if (bodyDied(state, genome, worldProperties.goblinPace)) {
            enqueueDeath(commands, goblin.entity, goblin.x, goblin.y);
            alive[g] = false;
            continue;
        }

        // Хромота проходит сама, тиками. Закон общий (core/Strike.hpp), и
        // отсчёт у гоблина такой же, как у зверя: срок кончился — нога
        // работает снова, и тяжесть возвращается к целой, чтобы следующий
        // укус не складывался с прошлым.
        if (goblin.injury->lameTicks > 0) {
            --goblin.injury->lameTicks;
            if (goblin.injury->lameTicks == 0) {
                goblin.injury->lameShare = kFull;
            }
        }

        // Память ног тает со временем, а не от шагов (core/Walk.hpp):
        // простоявший сотню тиков у куста не должен помнить преграду,
        // которой давно нет.
        fadeWalkMemory(*goblin.memory);

        // Усталость прибывает от того, что гоблин жив. Шаг добавит своё
        // ниже, в фазе шагов, а отдых вычтет своё в фазе решений: и то, и
        // другое — следствия того, чем он занят, и считать их здесь, до
        // выбора занятия, было бы гаданием.
        // Усталость — срок по той же причине, что и позыв к паре.
        if (paceBeat(tick, goblin.id, worldProperties.goblinPace)) {
            tireBy(goblin.tired->fatigue, kFatigueTick);
        }

        // Память тает сама. Не изнашивание и не уборка: именно забывание и
        // заставляет возвращаться — помни гоблин вечно, ему хватило бы
        // одного обхода мира на всю жизнь (core/Knowledge.hpp).
        forget(*goblin.mind);

        // И знакомства остывают — тем же законом и по той же причине
        // (core/Bonds.hpp). Срок и смещение по идентификатору внутри: иначе
        // все знакомства мира обрывались бы одним и тем же тиком.
        coolBonds(*goblin.bonds, tick, goblin.id);

        // Тоска по разговору копится у всякого живого — срок, а не дробь, по
        // той же причине, что и позыв к паре. Скорость своя у каждого: она и
        // есть общительность (talkStep, core/Character.hpp), и молчун (нрав 0)
        // не заговорит первым никогда.
        //
        // Копится она и у того, кому не с кем говорить: тоска не спрашивает,
        // есть ли рядом кто-нибудь. А вот ЖЕЛАНИЕМ она станет только при
        // собеседнике (chooseGoblinDesire) — накопленное же не пропадает, и
        // вышедший к своим после долгого одиночества заговаривает сразу.
        if (paceBeat(tick, goblin.id, worldProperties.goblinPace)) {
            desire.talking = std::min(
                kFull, desire.talking + talkStep(worldProperties.goblinTalkUrge, goblin.nature->sociable));
        }

        goblin.hunger = hungerOf(state, genome);
        goblin.thirst = thirstOf(state, genome);

        // Страх — общим законом (core/Fear.hpp), тем же, каким боится зверь.
        // Источников у гоблина два из трёх: видимые зубы и собственные раны.
        //
        // Падали среди них нет, и это выбор, а не пропуск: место, где кого-то
        // съели, уже отнимает у гоблина годность для лежания
        // (kRestCarcassPenalty, core/Rest.hpp) — он туда не ляжет и без
        // страха. Добавь сюда дрожь над тушей — и первая же смерть от
        // старости в лагере согнала бы всё поселение с обжитого места,
        // причём дважды одним и тем же поводом.
        //
        // Оба источника считаются в ОДНОМ переборе, в отличие от зверя, и
        // причина простая: у зверя это разные списки — зубы у хищников,
        // добить может всякий не своей диеты, — а у гоблина враг один и тот
        // же зверь с зубами. Двух списков нет, значит нет и двух переборов.
        goblin.fear = 0;
        {
            const int sightCells = std::max(1, genome.perception);
            const float sight = static_cast<float>(sightCells);
            const int mySize = std::max(1, bodySize(state, genome));
            const int hurt = kFull - std::clamp(state.health, 0, kFull);
            for (std::size_t b = 0; b < beasts.size(); ++b) {
                const int dx = beasts[b].x - goblin.x;
                const int dy = beasts[b].y - goblin.y;
                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (distance > sight) {
                    continue;
                }
                int scare = seenScare(distance, sight, kDesireFloor, beasts[b].size, mySize);
                if (hurt > 0) {
                    // Раненый боится не приближения, а самого присутствия:
                    // расстояние в эту величину не входит (core/Fear.hpp).
                    scare = std::max(scare, woundScare(hurt, beasts[b].size, mySize));
                }
                if (scare > goblin.fear) {
                    goblin.fear = scare;
                    threatX[g] = beasts[b].x;
                    threatY[g] = beasts[b].y;
                    threatBeast[g] = static_cast<int>(b);
                    hasThreat[g] = true;
                }
            }
        }

        // Где было страшно — то и запоминается. Единственное место, которое
        // помнят, чтобы обходить, а не чтобы прийти (PlaceKind::Danger).
        //
        // Порог тот же, с которого страх вообще становится желанием: ниже
        // него гоблина ничто никуда не гонит, и запоминать нечего. Ложится
        // испуг твёрже обычного места (kScareMark) — иначе у обжившегося
        // гоблина он не лёг бы вовсе: remember занимает чужой слот только
        // тем, что твёрже занятого.
        if (goblin.fear >= kDesireFloor) {
            remember(*goblin.mind, PlaceKind::Danger, goblin.x, goblin.y, kScareMark);
        }

        const bool adult = state.age >= maturityAgeOf(genome, worldProperties.goblinPace) &&
                           state.growth >= kBreedingGrowth;
        grown[g] = adult ? 1 : 0;
        const bool content = state.health >= kFull && goblin.hunger < kCalmNeed && goblin.thirst < kCalmNeed;
        // Готов ли платить за роды: не отдыхает после прошлых и накопил
        // крупиц на целого ребёнка. Закон общий со зверем (AnimalSystem):
        // тело у них одно, и цена потомства у него одна.
        const bool canBearYoung = state.recovery == 0 && state.protein >= proteinNeedOf(genome);
        if (adult && content && canBearYoung) {
            // Позыв к паре — СРОК, а не деление: черта живёт в 1..10, и
            // десятая доля любого её значения — ноль, то есть "никогда"
            // (core/Scale.hpp). Раз в N тиков прибавляется целиком, и
            // средняя скорость выходит та же.
            if (paceBeat(tick, goblin.id, worldProperties.goblinPace)) {
                desire.mating = std::min(kFull, desire.mating + genome.breedingUrge);
            }
        }
        // Дом — вспомненное место отдыха. Спрашивается здесь, до выбора
        // занятия: без дома запасать некуда, и желание не должно побеждать.
        const bool hasHome = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y) != nullptr;

        // Насколько гоблина гонит строить. Складывается из двух вещей, и обе
        // честно ограничены тем, что он может знать (02_CorePrinciples.md,
        // п.6): чего не хватает ЗДЕСЬ, где он стоит, и есть ли начатое дело,
        // которое он помнит или видит.
        int building = 0;
        {
            const std::size_t here = index(goblin.x, goblin.y);
            // Первая причина — недовольство обжитым местом. Считается только
            // стоя на нём: недостаток чувствуют, а не вычисляют издалека.
            // Гоблин, лежащий на голой земле в месте, куда он ходит спать
            // каждый день, — и есть тот, кто начинает стройку.
            //
            // Мерится нехватка тем, СКОЛЬКО ПРИБАВИТ постройка (betterBuild),
            // а не тем, насколько здесь плохо, — и это не оттенок мерки, а
            // разница между "строят иногда" и "не строят никогда".
            //
            // Прежде стояло "kRestGood минус годность, но не больше прибавки".
            // Мерка выглядела разумной и была мертва по построению: домом
            // гоблин зовёт место, куда ЛЁГ, а ложится он только там, где
            // годность уже выше kRestGood, — значит, разность почти всегда
            // ноль или десятки. Порог желаний при этом 350: чтобы стройка
            // победила, годность дома должна была упасть ниже семидесяти, то
            // есть практически только от туши под боком. Замер это и
            // показывал: площадки ставились почти исключительно по второй
            // причине, куче под открытым небом.
            //
            // "Сколько прибавит" — ответ и на ту беду, ради которой прежняя
            // мерка обрезалась прибавкой: в годность входит штраф за тушу
            // (kRestCarcassPenalty, core/Rest.hpp), а её ни навес, ни
            // подстилка не убирают. Гоблин у обустроенной до предела лежанки,
            // на которую легла падаль, приходил бы строить и не находил бы
            // что. Гонит его не то, что здесь плохо, а то, что он может
            // здесь поправить.
            //
            // Нрав кладётся на эту величину так же, как на позыв доводить
            // начатое (keenOnWork ниже), и по той же причине: "сколько
            // прибавит навес" — это цена работы, а браться ли за работу такой
            // цены, решает трудолюбие. Прежний довод "нрав не умножает
            // нехватку" остаётся верным про НЕХВАТКУ — то, чего в мире
            // недостаёт, нравом не искажают, — но мерка теперь другая.
            // Числами: навес на голой поляне прибавляет 400, подстилка 250;
            // с размахом нрава (±450) трудолюбивый доводит дом до обоих,
            // средний ставит навес, ленивый не строит вовсе — характер видно
            // на карте, а не в панели.
            const auto* home = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y, kRestReturn);
            if (home != nullptr && home->x == goblin.x && home->y == goblin.y) {
                const RestPlace place{tiles.moisture[here], tiles.rockiness[here], tiles.treeAt[here] != 0,
                                       tiles.carcassMeat[here], tiles.trampled[here], tiles.canopy[here],
                                       tiles.bedding[here]};
                // Ноль при "нечего строить" обязателен: workUrgeOf — сдвиг, а
                // не множитель, и на пустом месте он дал бы трудолюбивому
                // четыре с половиной сотни позыва строить там, где строить
                // нечего.
                const BuildChoice better = betterBuild(place);
                building = better.kind == BuildKind::None
                               ? 0
                               : workUrgeOf(*goblin.nature, better.gain, Topic::Work);
                // Вторая причина — куча под открытым небом. Еда портится, и
                // это видно тому, кто стоит рядом с ней. Крыша над кучей и
                // есть склад (core/Store.hpp), отдельной постройки для него
                // не нужно.
                if (tiles.canopy[here] < kFull) {
                    const int uncovered = tiles.storeFood[here] * (kFull - tiles.canopy[here]) / kFull;
                    building = std::max(building, std::min(kFull, uncovered * kFull / kStoreShelterFull));
                }
                // Третья причина — открытый край и зубы, которые сюда
                // приходят. Обе половины гоблин знает честно: открытость он
                // видит под ногами (core/Bound.hpp), а опасность помнит
                // (PlaceKind::Danger).
                //
                // Гонит его ОТКРЫТОСТЬ, а память об опасности — гейт: она
                // делает вопрос осмысленным, но силы ему не задаёт. Тот же
                // приём, каким гасится желание запасать без дома.
                //
                // Перемножать их было первой попыткой, и она измерена: память
                // об опасности живёт на сотне из тысячи (её теснят из головы
                // места посильнее), и произведение выходило вчетверо ниже
                // порога желаний — нехватка не могла победить НИКОГДА, и за
                // шесть тысяч тиков не встало ни одной площадки.
                //
                // Так оно и правильнее по сути. Нехватка — это то, чего
                // недостаёт: открытая сторона. Насколько страшно — не мера
                // нехватки, а условие, при котором открытая сторона вообще
                // становится бедой; без зубов городиться незачем, мир и так
                // пуст.
                const auto* scary = recall(*goblin.mind, PlaceKind::Danger, goblin.x, goblin.y);
                if (scary != nullptr) {
                    building = std::max(building, rimHere(goblin.x, goblin.y, scary->x, scary->y).openness);
                }
            }
            // Начатое надо доводить: незаконченный замысел держит сам по
            // себе, без всякой нехватки. Помнит гоблин свою площадку или
            // видит чужую — разницы нет, вкладываться можно во всякую.
            //
            // Вот этот, постоянный, позыв нрав и правит — в отличие от
            // нехватки выше: доводить ли начатое, зависит от того, кто ты, а
            // прохудившийся навес прохудился у всех одинаково.
            const int keenOnWork = workUrgeOf(*goblin.nature, kBuildUrge, Topic::Work);
            if (building < keenOnWork && recall(*goblin.mind, PlaceKind::Work, goblin.x, goblin.y) != nullptr) {
                building = keenOnWork;
            }
        }
        // Есть ли поблизости тот, к кому стоит подойти. Без этого тоска
        // остаётся тоской и желанием не становится (core/Talk.hpp).
        //
        // Спрашивается ровно тем же законом, каким ниже (п.4) выбирается
        // собеседник, а не более дешёвым "видно ли кого-нибудь": видеть можно
        // и того, к кому идти не стоит, и тогда гоблин выбрал бы разговор, а
        // в п.4 не нашёл бы с кем, — и застрял бы в нём, потому что тоска не
        // тратится, а инерция держит.
        const Talker talker{goblin.id,           goblin.x,       goblin.y,
                            std::max(1, genome.perception), genome.species, goblin.nature->loyal};
        std::uint64_t talkRandom = mixSeed(mixSeed(goblin.id, tick), goblinSeed);
        const bool companionNear =
            chooseCompanion(talker, companions, *goblin.bonds, mind, talkRandom, sights).found;
        // Жребий разума — свой, отдельный от того, которым в п.4 разыгрываются
        // связки. Числа те же (seed мира, тик, имя гоблина), но сложены иначе:
        // два розыгрыша одного существа в один тик, начатые с одного
        // состояния, дали бы одно и то же число — и выбор занятия совпадал бы
        // с выбором клетки из равных.
        std::uint64_t mindRandom = mixSeed(mixSeed(goblinSeed, tick), goblin.id);
        desire.current = chooseGoblinDesire(goblin, adult && content && canBearYoung, hasHome, building,
                                             companionNear,
                                             mind, mindRandom);
    }

    // Возможная пара в том виде, в каком её видно со стороны
    // (core/Mating.hpp). Желания у всех уже посчитаны (п.3), поэтому
    // "согласен" здесь честное, а не прошлотиковое.
    //
    // Хищником не назван никто: закон встречи различает диеты, потому что у
    // животных ими различаются виды, а гоблины все одной таблицы —
    // различает их племя, и оно едет в поле species.
    // Кто из стоявших рядом дожил до решений. Правится здесь, а не при
    // сборке списка: до п.3 этого не знал никто.
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        companions[g].alive = alive[g];
    }

    std::vector<MateCandidate> mates;
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        mates.push_back(MateCandidate{goblins[g].id, goblins[g].x, goblins[g].y, goblins[g].genome->species,
                                       false, goblins[g].state->sex,
                                       alive[g] && goblins[g].desire->current == GoblinDesire::Mate,
                                       alive[g] && grown[g] != 0});
    }

    // Округа и дорога по ней (core/Path.hpp). Живут снаружи цикла и
    // переиспользуются: за тик волна пускается столько раз, сколько в мире
    // ищущих, а массивы у неё на всю Область.
    Reach reachOf;
    std::vector<PathCell> road;

    // --- 4. Решения: что гоблин делает со своим желанием ---
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        if (!alive[g]) {
            continue;
        }
        const Goblin& goblin = goblins[g];
        auto& state = *goblin.state;
        const auto& genome = *goblin.genome;
        auto& desire = *goblin.desire;
        const std::size_t here = index(goblin.x, goblin.y);
        const int size = bodySize(state, genome);

        // Случайность собирается из seed мира, номера тика и постоянного
        // идентификатора (core/Random.hpp). Не из координат: клетка меняется
        // каждый шаг, а на одной клетке их может стоять несколько — розыгрыш
        // вышел бы одинаковым.
        std::uint64_t random = mixSeed(goblinSeed, mixSeed(tick, goblin.id));

        const int reach = std::max(1, genome.perception);

        // Где ходит ровня — запоминается ВСЕГДА, а не тогда, когда ищешь
        // пару (PlaceKind::Mate). Занятие тут ни при чём: гоблин видит, кто
        // ходит по этой поляне, пока ест, работает и разговаривает, — и
        // помнит это к тому дню, когда понадобится.
        //
        // Пока помнилось только место состоявшейся встречи, памяти этой не
        // было почти ни у кого: встречи редки, а до первой из них мужчина не
        // помнил ни одного места вовсе — и, не видя рядом согласной, уходил
        // блуждать наугад через полкарты. Уходил тем дальше, чем дольше
        // искал: блуждание держит направление сотню тиков (kRoamTicks) и
        // назад не возвращает.
        //
        // Согласие для памяти не спрашивается (mateKind, core/Mating.hpp), и
        // это главное: согласие живёт один тик и совпадает у двоих редко, а
        // "здесь ходят женщины" — правда надолго. Разговор в это правило
        // входит сам собой: говорят рядом, то есть в виду, — а тепло от
        // разговора кладёт место твёрже (kMateMark).
        if (grown[g] != 0) {
            const Suitor seer{goblin.id, goblin.x, goblin.y, reach, genome.species, false, state.sex};
            if (const MateChoice seen = sightOfMate(seer, mates, goblin.bonds); seen.found) {
                remember(*goblin.mind, PlaceKind::Mate, seen.x, seen.y,
                         kMetMark + warmthTo(*goblin.bonds, seen.id) * kMateMark / kFull);
            }
        }

        bool busy = false;
        bool hasTarget = false;
        int targetX = goblin.x;
        int targetY = goblin.y;

        // Куда гоблин вообще может встать (core/Path.hpp, standableAt): не за
        // границей Области, не в воду — и вот дальше начинается разница со
        // зверем. Высокогорье, валун и дерево гоблин берёт РУКАМИ (kOnHands,
        // core/Climb.hpp), и это единственная строчка во всей системе, где
        // сказано, чем он лезет.
        //
        // Отсюда и всё остальное: волна дороги у него шире звериной, вершины
        // для него не преграда, а на валун он влезает. Вода при этом остаётся
        // стеной и ему — за неё не ухватишься.
        auto standable = [&](int nx, int ny) {
            if (!world.area().inBounds(nx, ny)) {
                return false;
            }
            const std::size_t cell = index(nx, ny);
            return standableAt(world.area().isBlocked(nx, ny), terrain[cell] != entt::null, waterAt[cell],
                               tiles.terrainHeight[cell], tiles.fenceAt[cell], kOnHands);
        };

        // Куда идти из того, что видно, — решает РАЗУМ (core/Mind.hpp). Мир
        // здесь только складывает варианты и вес каждого: чем ближе клетка,
        // тем вес больше. Кто из них будет выбран — не его дело.
        //
        // Прежде выбор был зашит: всегда ближайшая, а из равноудалённых —
        // жребий. Жребий никуда не делся, он переехал в разум и стал третьей
        // ступенью общего правила (вес, старшинство, жребий), а "всегда
        // ближайшая" перестало быть законом мира и стало повадкой ОДНОГО
        // разума: жадный по-прежнему берёт ближайшую, жребий чаще берёт
        // ближайшую, но не всегда.
        //
        // Вес считается так, чтобы самая дальняя видимая клетка получила
        // единицу, а не ноль: нулевой вес значил бы "этого варианта нет", а
        // он есть — он просто далеко.
        auto findNearest = [&](auto predicate, int& outX, int& outY) {
            sights.clear();
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int nx = goblin.x + dx;
                    const int ny = goblin.y + dy;
                    if (!world.area().inBounds(nx, ny)) {
                        continue;
                    }
                    const int distance = dx * dx + dy * dy;
                    if (distance > reach * reach) {
                        continue; // видимость круглая, а не квадратная
                    }
                    if (!predicate(index(nx, ny), nx, ny)) {
                        continue;
                    }
                    sights.push_back(Option{0, nx, ny, reach * reach + 1 - distance, 0, false, 0});
                }
            }
            const Choice choice = decide(mind, sights, Temper{}, random);
            if (!choice.made) {
                return -1;
            }
            outX = choice.x;
            outY = choice.y;
            const int dx = choice.x - goblin.x;
            const int dy = choice.y - goblin.y;
            return dx * dx + dy * dy;
        };

        // Нужного не видно — идём туда, где оно было. Общее окончание всех
        // трёх веток: сперва разочароваться, если стоим ровно на том
        // вспомненном месте, где ничего нет, потом вспомнить лучшее.
        //
        // Идут к вспомненному НАПРЯМИК, а не дорогой (core/Path.hpp), и это
        // не упрощение: дорога считается волной в пределах видимости, а
        // вспомненное место лежит дальше. Гоблин помнит, ГДЕ, но не помнит,
        // КАК, — преграду он обойдёт вслепую памятью ног, как делает это,
        // идя за травой.
        const auto goByMemory = [&](PlaceKind kind, int minScore = 0) {
            if (const auto* known = recall(*goblin.mind, kind, goblin.x, goblin.y, minScore)) {
                if (known->x == goblin.x && known->y == goblin.y) {
                    // Пришли, а нужного нет: место обмануло.
                    disappoint(*goblin.mind, kind, goblin.x, goblin.y);
                    return false;
                }
                targetX = known->x;
                targetY = known->y;
                return true;
            }
            return false;
        };

        switch (desire.current) {
            case GoblinDesire::Food: {
                // Еда В РУКАХ — раньше всего остального, потому что она уже
                // в руках: за ней не надо ни идти, ни делить её с соседом.
                //
                // Отсюда и честная плата за дорогу: несущий добычу через
                // полкарты рискует съесть её сам, и доносит запас тот, кто
                // вышел сытым. Ничего специально для этого не написано —
                // просто голод сильнее желания запасать (см. kHaulUrge).
                if (goblin.hands->carried.of(ResourceKind::Food) > 0) {
                    const Portion bite =
                        takeFromHands(*goblin.hands, ResourceKind::Food, paced(genome.biteSize * size / kFull, worldProperties.goblinPace));
                    feedBody(state, genome, bite.amount, kEnergyPerBiomass);
                    takeProtein(state, genome, bite.minerals);
                    busy = true;
                    break;
                }
                // Куча под ногами — вторая: она в известном месте и никуда не
                // денется, но за ней всё же надо было дойти.
                if (storeFood[here] > kMinBiteGrowth) {
                    scoops.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, paced(genome.biteSize * size / kFull, worldProperties.goblinPace)});
                    remember(*goblin.mind, PlaceKind::Food, goblin.x, goblin.y);
                    busy = true;
                    break;
                }
                // Мясо под ногами — раньше травы под ногами: туша это
                // десяток кустов разом (kMeatPerSize, core/Carcass.hpp), и
                // пренебречь ею ради пучка травы значило бы оставить её
                // гнить. Живое гоблин при этом не бьёт — он подбирает
                // мёртвое.
                if (carcassMeat[here] > kMinBiteMeat) {
                    meals.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, paced(genome.biteSize * size / kFull, worldProperties.goblinPace)});
                    // Помнится то, что ПРИГОДИЛОСЬ, а не то, что попалось на
                    // глаза (core/Knowledge.hpp).
                    remember(*goblin.mind, PlaceKind::Food, goblin.x, goblin.y);
                    busy = true;
                    break;
                }
                // Ягоды с куста под ногами. Рвать, а не объедать: куст от
                // сбора не убывает и останется стоять (core/Berries.hpp).
                // Сколько ягод за тик — от размера тела, как и укус: рук у
                // взрослого больше, чем у ребёнка.
                if (bushAt[here] != entt::null && berriesAt[here] > 0) {
                    picks.push_back(ShareIntent{here, static_cast<int>(g), goblin.id,
                                                 std::max(1, kBerryPick * size / kFull)});
                    // Помнится то, что ПРИГОДИЛОСЬ, а не то, что попалось на
                    // глаза (core/Knowledge.hpp).
                    remember(*goblin.mind, PlaceKind::Food, goblin.x, goblin.y);
                    busy = true;
                    break;
                }
                // Трава — голодный запас, и только он. Гоблин щиплет её,
                // раз уж стоит на ней, но НЕ ЗАПОМИНАЕТ этого места: голова
                // у него на восемь мест (core/Knowledge.hpp), трава растёт
                // везде, и первая же съеденная травинка вытеснила бы из
                // памяти ягодник — единственное, к чему стоит возвращаться.
                if (plantAt[here] != entt::null && edibleGrowth(plantGrowth[here]) > kMinBiteGrowth) {
                    bites.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, paced(genome.biteSize * size / kFull, worldProperties.goblinPace)});
                    busy = true;
                    break;
                }

                // Под ногами пусто — СНАЧАЛА ВСПОМНИТЬ, и только потом
                // искать. Порядок этот — про то, чем гоблин отличается от
                // зверя, а не про экономию перебора.
                //
                // Пока глаз шёл раньше памяти, гоблин каждый раз сворачивал к
                // первому попавшемуся кусту, а к своему ягоднику возвращался
                // только тогда, когда вокруг не было ничего вовсе, — то есть
                // память работала последним средством, а маршрут оставался
                // случайным. Спросив голову первой, он идёт СВОЕЙ дорогой
                // мимо чужих кустов, а глазами пользуется там, где головой
                // пусто: в незнакомом месте и на первых днях жизни. Из этого
                // и набивается тропа.
                //
                // Обманувшее место при этом не держит: придя и не найдя,
                // гоблин теряет к нему веру (disappoint внутри goByMemory), и
                // после второго пустого прихода память молчит — тогда глаз и
                // получает своё.
                hasTarget = goByMemory(PlaceKind::Food);

                // Сначала падаль: она редка, лежит в одной точке, и идти к
                // ней надо дорогой (core/Path.hpp), иначе увиденная через
                // реку заведёт гоблина на берег и оставит там. Волна
                // считается только тогда, когда есть на что смотреть:
                // перебор клеток дёшев, а волна по округе — нет.
                int meatX = goblin.x;
                int meatY = goblin.y;
                const bool meatSeen =
                    !hasTarget && findNearest([&](std::size_t cell, int nx, int ny) {
                        return carcassMeat[cell] > kMinBiteMeat && standable(nx, ny);
                    }, meatX, meatY) >= 0;
                if (meatSeen) {
                    reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                    if (reachOf.reached(meatX, meatY)) {
                        reachOf.roadTo(meatX, meatY, road);
                        if (!road.empty()) {
                            targetX = road.front().x;
                            targetY = road.front().y;
                            hasTarget = true;
                        }
                    }
                }

                // Потом ягодник — и к нему тоже ДОРОГОЙ, по той же причине,
                // что и к падали: куст стоит в одной точке, их на карте
                // мало, и увиденный через реку увёл бы гоблина на берег
                // ждать. За травой так не ходят, а за ягодами ходят — в этом
                // и разница между фоном и местом.
                if (!hasTarget) {
                    int berryX = goblin.x;
                    int berryY = goblin.y;
                    const bool berriesSeen =
                        findNearest([&](std::size_t cell, int nx, int ny) {
                            return bushAt[cell] != entt::null && berriesAt[cell] > 0 && standable(nx, ny);
                        }, berryX, berryY) >= 0;
                    if (berriesSeen) {
                        reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                        if (reachOf.reached(berryX, berryY)) {
                            reachOf.roadTo(berryX, berryY, road);
                            if (!road.empty()) {
                                targetX = road.front().x;
                                targetY = road.front().y;
                                hasTarget = true;
                            }
                        }
                    }
                }

                // Трава — последняя и только та, что видно рядом. Идут к ней
                // напрямик, а преграду обходят вслепую памятью ног: упираться
                // в берег ради пучка травы незачем.
                if (!hasTarget) {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return plantAt[cell] != entt::null &&
                                               edibleGrowth(plantGrowth[cell]) > kMinBiteGrowth &&
                                               standable(nx, ny);
                                    },
                                    targetX, targetY) >= 0;
                }
                break;
            }
            case GoblinDesire::Water: {
                // Пьёт со своей клетки или с любой соседней: гоблин стоит на
                // берегу, а не заходит в реку — шагнуть в воду он и не может
                // (см. standable). Своя клетка в проверке всё равно нужна:
                // паводок может залить ту, на которой он стоит.
                std::size_t source = cellCount;
                if (waterAt[here] > 0) {
                    source = here;
                } else {
                    for (int dir = 0; dir < 8; ++dir) {
                        const int nx = goblin.x + kWalkX[dir];
                        const int ny = goblin.y + kWalkY[dir];
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
                        ShareIntent{source, static_cast<int>(g), goblin.id, paced(kDrinkRate * size / kFull, worldProperties.goblinPace)});
                    // Помнится берег, на котором стоял, а не сама вода: в
                    // воду гоблин шагнуть не может, и место водопоя — это
                    // клетка под ногами.
                    remember(*goblin.mind, PlaceKind::Water, goblin.x, goblin.y);
                    busy = true;
                } else {
                    // Вспомненный водопой — раньше увиденной воды, как и у
                    // еды: гоблин идёт к своему берегу, а глазами ищет
                    // только тогда, когда своего берега не помнит (см.
                    // порядок в ветке еды). Лужа под боком от этого не
                    // пропадает — она найдётся сама, когда память промолчит
                    // или обманет.
                    hasTarget = goByMemory(PlaceKind::Water);
                    if (!hasTarget) {
                        hasTarget = findNearest(
                                        [&](std::size_t cell, int, int) { return waterAt[cell] > 0; },
                                        targetX, targetY) >= 0;
                    }
                }
                break;
            }
            case GoblinDesire::Mate: {
                // Пару ищут дорогой (core/Mating.hpp): увиденное через реку —
                // ещё не найденное. Пара за водой видна обоим, сойтись им
                // негде, и оба стоят до конца жизни в двадцати шагах друг от
                // друга.
                const Suitor suitor{goblin.id, goblin.x, goblin.y, reach, genome.species, false, state.sex};
                if (!anyMateInSight(suitor, mates)) {
                    // Рядом никого не видно — но зовущая слышна дальше, чем
                    // видна. Цель ставится прямо, без дороги: звук не
                    // спрашивает брода, а как дойти, решит сам шаг.
                    const MateChoice call = hearCall(suitor, mates, mind, random, sights);
                    if (call.found) {
                        targetX = call.x;
                        targetY = call.y;
                        hasTarget = true;
                        break;
                    }
                    // Согласной не видно — но ровня-то, может, и видна,
                    // просто занята едой или работой (mateKind,
                    // core/Mating.hpp). К ней и подойти: согласие живёт один
                    // тик и совпадает у двоих редко, а стоящий рядом дождётся
                    // его первым.
                    //
                    // Это же и лекарство от блуждания: пока в виду есть хоть
                    // кто-то, за парой незачем уходить с поляны. Дорога здесь
                    // нужна ровно по той же причине, что и при встрече: ждать
                    // через реку значит ждать зря.
                    if (anyMateInSight(suitor, mates, false)) {
                        reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                        const MateChoice near = chooseMate(reachOf, suitor, mates, mind, random, sights, goblin.bonds, false);
                        if (near.found) {
                            reachOf.roadTo(near.x, near.y, road);
                            if (!road.empty()) {
                                targetX = road.front().x;
                                targetY = road.front().y;
                                hasTarget = true;
                            } else {
                                // Дороги нет только в одном случае: она уже
                                // тут, на этой самой клетке. Тогда ждать —
                                // это и есть занятие, и уходить с места
                                // блуждать было бы прямой потерей: согласие
                                // придёт к ней тиком позже, а его тут уже не
                                // будет.
                                busy = true;
                            }
                            break;
                        }
                    }
                    // Не видно никого — идти туда, где ровня ходила прежде
                    // (PlaceKind::Mate). До этого места здесь кончалось всё:
                    // гоблин с сильнейшим желанием оставался стоять или
                    // уходил блуждать, а зов подаёт только женщина — мужчине
                    // с пустой округой ждать было нечего.
                    //
                    // Опустевшую поляну отпустит disappoint, как и всякое
                    // другое место: пришёл, никого нет — веры меньше.
                    hasTarget = goByMemory(PlaceKind::Mate);
                    if (hasTarget) {
                        break;
                    }

                    // И последнее, вместо поля: идти к ЛЮДЯМ — к любому
                    // живому, кто в виду, будь он хоть чужого племени, хоть
                    // того же пола.
                    //
                    // Это не поиск пары, а знакомство, и разница видна на
                    // карте. Ищущий пару, которому некого выбрать, уходил
                    // блуждать: направление берётся жребием на сотню тиков
                    // (kRoamTicks) и назад не возвращает — гоблин уходил в
                    // поле мимо своих же соседей, потому что ровни среди них
                    // в этот тик не нашлось. А пары заводятся там, где люди:
                    // придя к ним, он и заговорит (тоска станет желанием,
                    // chooseGoblinDesire), и потеплеет, и увидит наконец
                    // ровню, когда она мимо пройдёт.
                    //
                    // Дороги здесь нет намеренно: цель мягкая, идти к ней
                    // можно и вслепую памятью ног — как ходят к вспомненному
                    // месту. Волна на "просто к людям" была бы платой не по
                    // товару.
                    {
                        std::size_t nearest = goblins.size();
                        int nearestSteps = 0;
                        for (std::size_t b = 0; b < goblins.size(); ++b) {
                            if (b == g || !alive[b]) {
                                continue;
                            }
                            const int dx = goblins[b].x - goblin.x;
                            const int dy = goblins[b].y - goblin.y;
                            if (dx * dx + dy * dy > reach * reach) {
                                continue;
                            }
                            const int steps = std::max(std::abs(dx), std::abs(dy));
                            if (steps == 0) {
                                continue; // уже вместе: стоять и так можно
                            }
                            // Из равно далёких — меньший идентификатор, а не
                            // первый в памяти (02_CorePrinciples.md, п.12a).
                            if (nearest != goblins.size() &&
                                (steps > nearestSteps ||
                                 (steps == nearestSteps && goblins[b].id > goblins[nearest].id))) {
                                continue;
                            }
                            nearest = b;
                            nearestSteps = steps;
                        }
                        if (nearest != goblins.size()) {
                            targetX = goblins[nearest].x;
                            targetY = goblins[nearest].y;
                            hasTarget = true;
                        }
                    }
                    break;
                }
                reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                // К кому идти — не только про расстояние: пара, к которой
                // тепло, стоит ближе, чем стоит на самом деле (chooseMate,
                // core/Mating.hpp). Симпатия здесь та же самая, что решает, с
                // кем гоблин заговорит, и заводить для пары второе чувство не
                // пришлось: тепло копится от встреч, а к паре ходят к тому, с
                // кем виделись.
                const MateChoice mate = chooseMate(reachOf, suitor, mates, mind, random, sights, goblin.bonds);
                if (!mate.found) {
                    break;
                }
                // Сошлись — встреча случилась на этой клетке. Кто с кем
                // именно, решится ниже (п.9), когда соберутся все: намерение
                // здесь не называет второго, потому что на одной клетке их
                // может ждать и трое.
                if (mate.x == goblin.x && mate.y == goblin.y) {
                    matings.push_back(
                        MateIntent{here, static_cast<int>(g), goblin.id, genome.species, state.sex});
                    // Отдельно запоминать место встречи не нужно: оно уже
                    // легло в голову вверху этого же тика — от того, что он
                    // её ВИДЕЛ. Один закон на "видел" и "сошёлся" лучше двух:
                    // сошедшийся видел заведомо, а два закона об одном
                    // разъезжаются молча (CLAUDE.md).
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
            case GoblinDesire::Rest: {
                // Годность клетки — общий закон (core/Rest.hpp): по нему же
                // наблюдатель рисует эту пригодность на карте.
                const auto placeAt = [&](std::size_t cell, int nx, int ny) {
                    return RestPlace{tiles.moisture[cell], tiles.rockiness[cell], tiles.treeAt[cell] != 0,
                                      carcassMeat[cell], tiles.trampled[cell], canopyAt[cell],
                                      beddingAt[cell]};
                };
                if (restQualityOf(placeAt(here, goblin.x, goblin.y)) >= kRestGood) {
                    // Лёг. Отдых — единственное занятие, которое НИЧЕГО не
                    // забирает у мира: гоблин просто не идёт никуда, и от
                    // этого ему становится легче. Оттого место для отдыха
                    // ничем и не кончается, в отличие от куста и туши.
                    // Тем же сроком, что и накопление усталости (см.
                    // AnimalSystem): подели одно и не подели другое — и лёжка
                    // станет вдесятеро действеннее, чем ходьба утомительна.
                    if (paceBeat(tick, goblin.id, worldProperties.goblinPace)) {
                        restBy(goblin.tired->fatigue, kRestRelief);
                    }
                    remember(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y);
                    busy = true;
                    break;
                }
                // ОБЖИТОЕ место — раньше любого годного в виду, и это
                // главное решение всего шага "места притяжения".
                //
                // Пока гоблин ложился на первой попавшейся годной клетке,
                // возвращаться ему было не к чему: годных клеток много, и та
                // же самая выпадала лишь по совпадению. Спросив сначала
                // память — и только твёрдую (kRestReturn, core/Knowledge.hpp),
                // — он идёт мимо годного к тому, где уже спал. Оттуда и
                // берётся лагерь: место, к которому возвращаются несколько
                // соседей, а не место, которое кто-то назначил.
                // Порога твёрдости здесь больше нет, и это то же решение,
                // что в еде и воде: у памяти спрашивают "помню ли я место",
                // а не "достаточно ли оно обжитое, чтобы идти мимо
                // видимого". Место, к которому ходили мало, всё равно
                // вероятнее случайной годной клетки, а переставшее быть
                // годным гоблин отпустит сам: придёт, не ляжет и потеряет к
                // нему веру (disappoint).
                hasTarget = goByMemory(PlaceKind::Rest);

                // Ближайшая годная, а не лучшая в округе: гоблин идёт к
                // тому, что видит рядом и что ему подходит. Выбирать лучшее
                // из всего круга видимости значило бы знать округу целиком.
                if (!hasTarget) {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return standable(nx, ny) &&
                                               restQualityOf(placeAt(cell, nx, ny)) >= kRestGood;
                                    },
                                    targetX, targetY) >= 0;
                }
                break;
            }
            case GoblinDesire::Haul: {
                // Дом — вспомненное место отдыха. Он здесь есть заведомо: без
                // него желание не побеждает вовсе (см. chooseGoblinDesire).
                const auto* home = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y);
                if (home == nullptr) {
                    break;
                }

                // Руки полны — домой. Пришёл — положил.
                if (carryRoom(*goblin.hands, size) <= 0) {
                    if (goblin.x == home->x && goblin.y == home->y) {
                        // Кладут столько, сколько влезает: у клетки есть
                        // предел (kStoreCapacity, core/Store.hpp). Не влезшее
                        // остаётся в руках и не пропадает — вещество в этом
                        // мире не исчезает оттого, что ему не хватило места.
                        // Гоблин с полными руками у полной кучи просто идёт
                        // дальше и рано или поздно съедает принесённое сам.
                        const int room = std::max(0, kStoreCapacity - storeTotal[here]);
                        const Portion give = takeFromHands(*goblin.hands, ResourceKind::Food, room);
                        if (give.amount <= 0) {
                            break;
                        }
                        // Класть — через очередь: компонент кучи может
                        // появиться, а это структурное изменение
                        // (05_Entity.md, п.5).
                        commands.enqueue([x = goblin.x, y = goblin.y, give](World& w) {
                            depositStore(w, x, y, ResourceKind::Food, give);
                        });
                        // Куча — это ЕДА, лежащая в известном месте, и
                        // помнится она именно так. Никакого "склада" как
                        // отдельного понятия в голове гоблина нет: голодный
                        // вспомнит это место наравне с ягодником и придёт
                        // сюда. Оттого лагерь и становится местом, куда
                        // возвращаются и спать, и есть.
                        remember(*goblin.mind, PlaceKind::Food, goblin.x, goblin.y);
                        busy = true;
                        break;
                    }
                    targetX = home->x;
                    targetY = home->y;
                    hasTarget = true;
                    break;
                }

                // Руки не полны — набирать. Ягоды под ногами идут в руки, а
                // не в рот: тем и отличается запасающий от голодного, что он
                // не ест.
                if (bushAt[here] != entt::null && berriesAt[here] > 0) {
                    picks.push_back(ShareIntent{here, static_cast<int>(g), goblin.id,
                                                 std::max(1, kBerryPick * size / kFull)});
                    remember(*goblin.mind, PlaceKind::Food, goblin.x, goblin.y);
                    busy = true;
                    break;
                }

                // Куда идти за ягодой — сперва по памяти, как и за едой для
                // себя: запасающий ходит на свой ягодник, а не на первый
                // попавшийся.
                hasTarget = goByMemory(PlaceKind::Food);

                // Не помнит — искать глазами, и дорогой: ягодник редок и
                // стоит в одной точке.
                int berryX = goblin.x;
                int berryY = goblin.y;
                const bool berriesSeen =
                    !hasTarget && findNearest([&](std::size_t cell, int nx, int ny) {
                        return bushAt[cell] != entt::null && berriesAt[cell] > 0 && standable(nx, ny);
                    }, berryX, berryY) >= 0;
                if (berriesSeen) {
                    reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                    if (reachOf.reached(berryX, berryY)) {
                        reachOf.roadTo(berryX, berryY, road);
                        if (!road.empty()) {
                            targetX = road.front().x;
                            targetY = road.front().y;
                            hasTarget = true;
                        }
                    }
                }
                break;
            }
            case GoblinDesire::Build: {
                // Что здесь недоделано: замысел или начатая постройка. Один
                // ответ на все вопросы ветки (core/Build.hpp) — иначе гоблин
                // ходил бы достраивать то, что для мира уже достроено.
                const BuildKind unfinished = unfinishedAt(
                    BuildingComponent{canopyAt[here], beddingAt[here]}, siteKind[here]);

                // --- 0. Замысел: недовольный местом отмечает клетку ---
                // Ставит его тот, кто на этом месте СТОИТ и кому здесь плохо
                // (см. срочность выше). Дальше замысел видно всякому, и
                // достраивать его будут сообща.
                if (unfinished == BuildKind::None) {
                    const auto* home = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y, kRestReturn);
                    if (home != nullptr && home->x == goblin.x && home->y == goblin.y) {
                        const RestPlace place{tiles.moisture[here], tiles.rockiness[here],
                                               tiles.treeAt[here] != 0, carcassMeat[here],
                                               tiles.trampled[here], canopyAt[here], beddingAt[here]};
                        const BuildKind kind = betterBuild(place).kind;
                        // Под деревом не строят — оно занимает клетку.
                        // placeSite откажет и сам, но незачем помнить как
                        // стройку то, чего не будет.
                        if (kind != BuildKind::None && tiles.treeAt[here] == 0) {
                            commands.enqueue([x = goblin.x, y = goblin.y, kind](World& w) {
                                placeSite(w, x, y, kind);
                            });
                            remember(*goblin.mind, PlaceKind::Work, goblin.x, goblin.y);
                            busy = true;
                            break;
                        }
                    }
                }

                // --- 0b. Кол на краю обжитого ---
                // Отдельным шагом, а НЕ внутри замысла выше, и это не
                // оформление: замысел стоит под условием "здесь ничего не
                // недоделано", а у обжитого лагеря навес почти всегда
                // подветшал — кол не встал бы никогда.
                //
                // Ставится он на СОСЕДНЮЮ клетку, в отличие от навеса и
                // подстилки: забор улучшает не то место, где стоишь, а то,
                // что за ним. Оттого его и нет в betterBuild — тот отвечает
                // на вопрос "чего не хватает здесь".
                //
                // Гоблин при этом не задумывает кольца и не знает о нём:
                // он затыкает открытую сторону СВОЕЙ лежанки, ближнюю к тому
                // месту, где его пугали. Кольцо получается оттого, что край
                // утоптанного пятна замкнут сам по себе, а ворота — оттого,
                // что тропа утоптана и внешней клеткой не считается
                // (core/Bound.hpp).
                {
                    const auto* home = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y, kRestReturn);
                    const auto* scary = recall(*goblin.mind, PlaceKind::Danger, goblin.x, goblin.y);
                    if (home != nullptr && home->x == goblin.x && home->y == goblin.y && scary != nullptr) {
                        const Rim rim = rimHere(goblin.x, goblin.y, scary->x, scary->y);
                        // Кол там уже стоит — второго не надо, надо доплести
                        // первый. Тогда ветка не занимает гоблина и валится
                        // ниже, к работе: шаг 2 сам найдёт глазами ближайшую
                        // недоделанную площадку, а она в соседней клетке.
                        //
                        // Без этой проверки гоблин втыкал бы кол каждый тик,
                        // пока стороны не кончатся, и не заплёл бы ни одного:
                        // замер дал двадцать шесть площадок разом при
                        // прочности один из ста.
                        const bool alreadySited =
                            rim.found && tiles.siteKind[index(rim.towardX, rim.towardY)] != BuildKind::None;
                        if (rim.found && !alreadySited) {
                            commands.enqueue([x = rim.towardX, y = rim.towardY](World& w) {
                                placeSite(w, x, y, BuildKind::Fence);
                            });
                            // Место кола запоминается как стройка — КООРДИНАТАМИ
                            // САМОГО КОЛА, а не своими. Сперва оно не
                            // запоминалось вовсе: рассуждение было "оно под
                            // боком, его видно глазами", и голову занимать
                            // незачем. Замер это опроверг — колья стояли, а
                            // прочность держалась на единице из ста: у гоблина
                            // не было НИ ОДНОЙ причины к ним возвращаться.
                            //
                            // Именно памятью о стройке достраиваются навесы
                            // (kBuildUrge выше), и второго способа доводить
                            // начатое в мире нет. Слот в голове — цена того,
                            // чтобы начатое доводилось.
                            remember(*goblin.mind, PlaceKind::Work, rim.towardX, rim.towardY);
                            busy = true;
                            break;
                        }
                    }
                }

                // --- 1. Стоим на недоделанном: работать или принести материал ---
                if (unfinished != BuildKind::None) {
                    // Место помнится как стройка: пока не доделано, сюда
                    // возвращаются — и не за тем, что здесь хорошо, а за тем,
                    // что здесь недоделано.
                    remember(*goblin.mind, PlaceKind::Work, goblin.x, goblin.y);

                    const bool material = storeMaterial[here] >= kMaterialPerWork ||
                                           materialIn(goblin.hands->carried) >= kMaterialPerWork;
                    if (material) {
                        // Работа. Тик труда — одна единица (core/Work.hpp);
                        // делается она в фазе исполнения, вместе со всеми
                        // остальными вкладами в эту же постройку.
                        works.push_back(StepIntent{static_cast<int>(g), goblin.x, goblin.y});
                        busy = true;
                        break;
                    }
                    // Материала нет ни в куче, ни в руках — идти ломать.
                }

                // --- 2. Руки полны — нести к стройке ---
                if (carryRoom(*goblin.hands, size) <= 0 ||
                    (materialIn(goblin.hands->carried) > 0 && unfinished == BuildKind::None)) {
                    // Своя стройка — раньше чужой, попавшейся на глаза: тот
                    // же порядок, что у еды и воды. Материал несут туда, где
                    // уже работали, а не туда, что ближе; достроенную
                    // площадку память отпустит сама, придя пустой
                    // (disappoint внутри goByMemory).
                    hasTarget = goByMemory(PlaceKind::Work);
                    if (hasTarget) {
                        break;
                    }
                    int siteX = goblin.x;
                    int siteY = goblin.y;
                    const bool siteSeen = findNearest([&](std::size_t cell, int nx, int ny) {
                        return unfinishedAt(BuildingComponent{canopyAt[cell], beddingAt[cell]},
                                             siteKind[cell]) != BuildKind::None &&
                               standable(nx, ny);
                    }, siteX, siteY) >= 0;
                    if (siteSeen) {
                        targetX = siteX;
                        targetY = siteY;
                        hasTarget = true;
                    }
                    break;
                }

                // --- 3. Руки не полны — ломать ближайшее ---
                // Ветка втрое ценнее соломины (core/Build.hpp), поэтому
                // дерево под ногами разбирается раньше травы.
                if (tiles.treeEntity[here] != entt::null && tiles.treeGrowth[here] > kHarvestMinGrowth) {
                    harvests.push_back(ShareIntent{here, static_cast<int>(g), goblin.id, kTwigHarvest});
                    busy = true;
                    break;
                }
                if (plantAt[here] != entt::null && plantGrowth[here] > kHarvestMinGrowth) {
                    harvests.push_back(ShareIntent{here, static_cast<int>(g), goblin.id, kStrawHarvest});
                    busy = true;
                    break;
                }
                // Под ногами пусто — искать глазами: сперва дерево, потом
                // траву.
                hasTarget = findNearest([&](std::size_t cell, int nx, int ny) {
                    return tiles.treeGrowth[cell] > kHarvestMinGrowth && standable(nx, ny);
                }, targetX, targetY) >= 0;
                if (!hasTarget) {
                    hasTarget = findNearest([&](std::size_t cell, int nx, int ny) {
                        return plantAt[cell] != entt::null && plantGrowth[cell] > kHarvestMinGrowth &&
                               standable(nx, ny);
                    }, targetX, targetY) >= 0;
                }
                // Ни того, ни другого не видно — идти к стройке: там хотя бы
                // видно, чего не хватает.
                if (!hasTarget) {
                    hasTarget = goByMemory(PlaceKind::Work);
                }
                break;
            }
            case GoblinDesire::Talk: {
                // К кому идти, решает нрав: постоянного тянет к знакомому,
                // непостоянного — к новому лицу (core/Talk.hpp).
                const Talker talker{goblin.id, goblin.x, goblin.y, reach, genome.species,
                                     goblin.nature->loyal};
                const TalkChoice companion =
                    chooseCompanion(talker, companions, *goblin.bonds, mind, random, sights);
                if (!companion.found) {
                    break;
                }
                // Уже рядом — разговор состоялся. С кем именно, названо прямо
                // в намерении: заговоривший выбрал сам, и переигрывать за него
                // ниже было бы подменой.
                if (withinTalk(goblin.x, goblin.y, companion.x, companion.y)) {
                    talks.push_back(TalkIntent{static_cast<int>(g), goblin.id, companion.id});
                    busy = true;
                    break;
                }
                // Идут прямо, без волны дороги, — в отличие от пары. Дорога
                // заведена там потому, что двое по разные стороны реки иначе
                // простояли бы так до смерти, не оставив потомства. Здесь цена
                // ошибки — скучающий гоблин, потоптавшийся у берега: разговор
                // и так проигрывает всякому другому желанию, и первый же голод
                // уведёт его прочь. Волна же пускалась бы куда чаще, чем за
                // парой, — болтать хотят все и почти всегда.
                targetX = companion.x;
                targetY = companion.y;
                hasTarget = true;
                break;
            }
            case GoblinDesire::Flee: {
                // Куда спасаться, здесь не решается: у бегущего нет цели в
                // том смысле, в каком она есть у идущего за травой.
                // Направление шага считается ниже, отдельно, — и там же
                // видно, что бежит гоблин НЕ ПРОЧЬ, а к своим.
                //
                // А вот дать сдачи можно прямо здесь: зубы в соседней клетке
                // — и это всё условие (core/Strike.hpp). Не ответ на укус и
                // не особое решение, а то же самое действие, каким хищник
                // бьёт добычу, просто повод другой. Меткость у гоблина куплена
                // геномом (hit_chance) и до сих пор не читалась ничем: бить
                // ему было некого и нечем.
                //
                // Занятым (busy) удар не делает, в отличие от еды и разговора:
                // отмахнувшийся продолжает уходить. Иначе гоблин застревал бы
                // вплотную к зубам, отмахиваясь до смерти, — а это уже не
                // спасение, а поединок, которого он не выигрывает.
                if (hasThreat[g] && threatBeast[g] >= 0 &&
                    strikeReaches(goblin.x, goblin.y, threatX[g], threatY[g])) {
                    blows.push_back(BlowIntent{static_cast<int>(g), threatBeast[g]});
                }

                // Уже наверху — значит, спасаться больше некуда и незачем:
                // гоблин сидит и пережидает. Без этого он слезал бы обратно,
                // едва выше идти станет некуда, и лазал бы вверх-вниз у самой
                // кромки, пока зверь ходит внизу.
                //
                // "Наверху" — это два разных места (core/Climb.hpp), и оба
                // спрашиваются об одном: достанут ли отсюда зубы. На дереве и
                // на валуне зверь стоит рядом и не дотягивается; в
                // высокогорье он не стоит вовсе.
                const bool upSomething =
                    world.area().isBlocked(goblin.x, goblin.y) || tiles.treeAt[here] != 0;
                if (upSomething || tiles.terrainHeight[here] > kLegCeiling) {
                    busy = true;
                    break;
                }

                // Не наверху — значит, надо туда добраться, и добраться
                // НОГАМИ, а не одним шагом. Правило "шагни выше, если выше
                // можно" здесь не работает, и это замер, а не догадка: пока
                // бегство было пошаговым, доля испуганных в убежище равнялась
                // доле спокойных до десятой — то есть вверх не бежал никто.
                // Причина простая: рельеф плавный, и одна соседняя клетка
                // почти никогда не выше здешней настолько, чтобы это что-то
                // решало.
                //
                // Ищется САМОЕ ВЫСОКОЕ место в пределах видимости, а из
                // одинаково высоких — ближайшее. Валун и дерево считаются
                // выше голой земли под ними (climbedHeightOf): наверху
                // оказывается не клетка, а тот, кто на неё влез.
                //
                // Выбирает РАЗУМ (core/Mind.hpp), и оба признака ложатся на
                // его правило без остатка: высота — вес, близость —
                // старшинство. Третьей ступенью, жребием, разум сам разводит
                // одинаково высокие и одинаково близкие; прежде этот жребий
                // стоял здесь отдельной дюжиной строк.
                //
                // Знания "волк не лазает" у гоблина при этом нет и не
                // заводится: он лезет как можно выше, потому что может. Что
                // зверь за ним не пойдёт — свойство мира, а не догадка
                // гоблина (02_CorePrinciples.md, п.6).
                {
                    const int hereHigh = climbedHeightOf(tiles.terrainHeight[here],
                                                          world.area().isBlocked(goblin.x, goblin.y),
                                                          tiles.treeAt[here] != 0);
                    sights.clear();
                    for (int dy = -reach; dy <= reach; ++dy) {
                        for (int dx = -reach; dx <= reach; ++dx) {
                            const int distance = dx * dx + dy * dy;
                            const int nx = goblin.x + dx;
                            const int ny = goblin.y + dy;
                            if (distance > reach * reach || !standable(nx, ny)) {
                                continue;
                            }
                            const std::size_t cell = index(nx, ny);
                            const int up = climbedHeightOf(tiles.terrainHeight[cell],
                                                            world.area().isBlocked(nx, ny),
                                                            tiles.treeAt[cell] != 0);
                            // Ниже, чем стоишь, — не спасение. Отсечка здесь,
                            // а не весом: вес отрицательным не бывает, а
                            // спускаться ради бегства незачем.
                            if (up < hereHigh) {
                                continue;
                            }
                            sights.push_back(Option{0, nx, ny, up - hereHigh + 1,
                                                     reach * reach + 1 - distance, false, 0});
                        }
                    }
                    const Choice choice = decide(mind, sights, Temper{}, random);
                    // Своя же клетка в список входит и вполне может выиграть:
                    // она не ниже себя самой и ближе всех. Это не находка, а
                    // отсутствие находки, и означает ровно одно — выше идти
                    // некуда.
                    if (choice.made && (choice.x != goblin.x || choice.y != goblin.y)) {
                        targetX = choice.x;
                        targetY = choice.y;
                        hasTarget = true;
                    }
                }
                break;
            }
            case GoblinDesire::Idle: break;
        }

        // --- Шаг ---
        // Занятый (ест, пьёт, сошёлся с парой) с места не сходит.
        if (busy) {
            continue;
        }

        // Скорость — тысячных клетки за тик (core/Scale.hpp): копится, пока
        // не наберётся целая клетка. Одна клетка за тик и не больше; отсюда
        // и потолок — невыбранный запас иначе рос бы без конца, а шагов от
        // этого не прибавлялось бы.
        state.stepProgress = std::min(state.stepProgress + genome.speed, 2 * kFull - 1);
        if (state.stepProgress < kFull) {
            continue;
        }
        state.stepProgress -= kFull;

        // Далеко ли дом. Спрашивается один раз на тик, а не внутри
        // roamDirection: та зовётся до трёх раз, а память у гоблина одна и
        // за эти три раза не меняется.
        //
        // Дом — вспомненное место отдыха, то же самое, которое служит
        // адресом ноше и площадкой стройке. Порога твёрдости здесь нет
        // намеренно: вопрос не "обжитое ли оно", а "есть ли куда
        // возвращаться".
        const KnownPlace* homePlace = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y);
        const bool farFromHome =
            homePlace != nullptr &&
            std::max(std::abs(homePlace->x - goblin.x), std::abs(homePlace->y - goblin.y)) > kHomeRange;

        // Направление поиска, когда желаемого не видно. Гоблин идёт в одну
        // сторону целый отрезок пути (kRoamTicks), а не топчется на месте.
        // Берётся из постоянного идентификатора и номера отрезка, поэтому
        // системе не нужно ничего помнить между тиками (05_Entity.md, п.3).
        //
        // А вот у того, кто ушёл дальше своей округи, жребия нет: он идёт
        // домой (kHomeRange). Блуждание ищет новое, и искать его имеет смысл
        // вокруг дома, а не всё дальше от него.
        auto roamDirection = [&]() {
            if (farFromHome) {
                return walkDirectionTo(goblin.x, goblin.y, homePlace->x, homePlace->y);
            }
            std::uint64_t roam = mixSeed(goblin.id, tick / kRoamTicks);
            return static_cast<int>(nextState(roam) % 8u);
        };

        // Тянет гоблина к своим — к ближайшему из своего племени, если тот
        // дальше kHerdKeep (core/Walk.hpp, WalkHerd). Закон тот же, что у
        // стада, и признак "свой" тот же по смыслу: у зверя вид, у гоблина
        // племя. Держаться вместе — свойство живого, а не звериное, и писать
        // его дважды было бы ошибкой того же рода, что две копии тела.
        //
        // Лагерь этому не помеха, а опора: место притяжения (дом, куча,
        // навес) собирает племя туда, где ему хорошо, а тяга к своим не даёт
        // разбредаться тем, у кого дома ещё нет.
        //
        // Своим при этом считается сперва ЗНАКОМЫЙ, и только потом
        // соплеменник. Это и есть та единственная строчка, ради которой связи
        // вообще видны на карте: пока тянуло к племени, поселение было
        // родовым по устройству мира, а не потому, что эти двадцать гоблинов
        // сжились. Теперь оно складывается из тех, кто друг друга знает, — и
        // может оказаться смешанным, если племена наговорились (kStrangerTribe
        // делает это дорогим, но не запрещает).
        //
        // Порог "свой" берётся общий с вестями (kNewsWarmth, core/Bonds.hpp):
        // шкала тепла одна, и заводить на ней второе имя для того же места
        // значило бы подкручивать два числа там, где хватает одного.
        WalkHerd herd;
        {
            const int sightCells = std::max(1, genome.perception);
            std::size_t closest = goblins.size();
            int closestWarmth = 0;
            int closestDistance = 0;
            std::size_t kin = goblins.size();
            int kinDistance = 0;
            for (std::size_t b = 0; b < goblins.size(); ++b) {
                if (b == g || !alive[b]) {
                    continue;
                }
                const int dx = goblins[b].x - goblin.x;
                const int dy = goblins[b].y - goblin.y;
                const int distance = dx * dx + dy * dy;
                if (distance <= kHerdKeep * kHerdKeep || distance > sightCells * sightCells) {
                    continue;
                }
                const int warmth = warmthTo(*goblin.bonds, goblins[b].id);
                if (warmth >= kNewsWarmth &&
                    (closest == goblins.size() || warmth > closestWarmth ||
                     (warmth == closestWarmth && distance < closestDistance))) {
                    closest = b;
                    closestWarmth = warmth;
                    closestDistance = distance;
                }
                if (goblins[b].genome->species != genome.species) {
                    continue;
                }
                if (kin != goblins.size() && distance >= kinDistance) {
                    continue;
                }
                kin = b;
                kinDistance = distance;
            }
            const std::size_t pull = closest != goblins.size() ? closest : kin;
            if (pull != goblins.size()) {
                herd =
                    WalkHerd{walkDirectionTo(goblin.x, goblin.y, goblins[pull].x, goblins[pull].y), kHerdPull};
            }
        }

        // Кого сторониться: того, чьи зубы видно сейчас, а если не видно
        // ничьих — того места, где пугали прежде (PlaceKind::Danger).
        //
        // Вот она, "первая причина держаться подальше", под которую тут
        // столько времени стояло пустое место. Чужое племя ею так и не стало:
        // соперник за траву — не повод обходить, а зубы — повод.
        //
        // Видимое бьёт помнимое, а не складывается с ним: направление в
        // WalkShy одно (core/Walk.hpp), и зверь, стоящий перед носом, важнее
        // места, где когда-то было страшно.
        //
        // Сторонение при этом НЕ бегство: гоблин не бросает своего дела, он
        // лишь предпочитает ту сторону, где зверя нет. Бросить дело его
        // заставит страх, и это отдельное желание.
        WalkShy shy;
        if (hasThreat[g]) {
            shy = WalkShy{walkDirectionTo(goblin.x, goblin.y, threatX[g], threatY[g]), kDangerShy};
        } else if (const auto* scary = recall(*goblin.mind, PlaceKind::Danger, goblin.x, goblin.y);
                   scary != nullptr) {
            shy = WalkShy{walkDirectionTo(goblin.x, goblin.y, scary->x, scary->y), kDangerShy};
        }

        // Куда гоблин хочет — ОДНО направление на все случаи движения, и
        // дальше шаг считается одинаково (core/Walk.hpp). Идущий к цели,
        // спасающийся, ищущий за пределами видимости и просто бродящий
        // отличаются только тем, откуда взялось это направление; обход
        // преграды получается сам.
        int aim = -1;
        if (desire.current == GoblinDesire::Flee) {
            // Спасается гоблин К СВОИМ, а не прочь, и это единственное, что
            // он может сделать с зубами: убежать он не в силах — он медленнее
            // и добычи, и хищника, — а хищник ищет отбившегося (kHuntCompany,
            // core/Hunting.hpp). Сойтись вместе значит перестать быть
            // отбившимся.
            //
            // Сперва — туда, куда зубам хода нет, если такое место видно
            // (цель выбрана выше, в ветке желания). Идти к нему ногами, а не
            // прыгать одним шагом: уступ есть только по краю полки, и до края
            // ещё надо дойти.
            if (hasTarget) {
                aim = walkDirectionTo(goblin.x, goblin.y, targetX, targetY);
            }
            // Лезть некуда — к своим. Направление берётся то же, что и тяга к
            // ним, второго поиска для этого не нужно: страх лишь превращает
            // слабую тягу в цель. Оттого испуганные и сбиваются в кучу там,
            // где до испуга просто держались рядом.
            //
            // Своих не видно — уходить прочь от зубов. Не спасение, но и не
            // стояние на месте.
            if (aim < 0) {
                aim = herd.direction;
            }
            if (aim < 0 && hasThreat[g]) {
                aim = walkDirectionTo(threatX[g], threatY[g], goblin.x, goblin.y);
            }
            if (aim < 0) {
                aim = roamDirection();
            }
        } else if (hasTarget) {
            aim = walkDirectionTo(goblin.x, goblin.y, targetX, targetY);
        } else if (desire.current != GoblinDesire::Idle) {
            aim = roamDirection();
        } else if (farFromHome) {
            // Ничего не гонит, а дом далеко — идти домой, и без жребия
            // "шагнуть или постоять". Жребий описывает бездельника НА МЕСТЕ:
            // постоянно бродящий выглядит нервным, неподвижный — мёртвым. У
            // того, кто далеко, занятие есть, и оно то самое, ради которого
            // дом и заводился.
            aim = walkDirectionTo(goblin.x, goblin.y, homePlace->x, homePlace->y);
        } else if (static_cast<int>(randomBelow(random, kFull)) >= kWanderChance) {
            continue; // ничего не гонит — стоит
        }

        // Восьмое слагаемое шага: по натоптанному идти легче
        // (core/Walk.hpp). Читается из снимка тика, как и всё остальное,
        // чтобы решения всех гоблинов принимались по одному состоянию мира.
        const auto trodden = [&](int nx, int ny) {
            return world.area().inBounds(nx, ny) ? tiles.trampled[index(nx, ny)] : 0;
        };
        const WalkStep step =
            chooseStep(*goblin.memory, goblin.x, goblin.y, aim, shy, herd, standable, trodden, random);
        if (!step.moved) {
            continue; // шагнуть некуда вовсе: вода, камень или край мира
        }

        // Шаг с ношей дороже пустого (core/Carry.hpp). Без этой платы носить
        // всегда было бы выгоднее, чем не носить, и решать тут было бы
        // нечего; с ней дальний ягодник окупается хуже ближнего — а решает
        // это не гоблин, а мир.
        state.energy =
            std::max(0, state.energy - carryStepEnergy(paced(kStepEnergy * size / kFull, worldProperties.goblinPace), *goblin.hands, size));
        // Шаг стоит не только энергии, но и сил: ходьба утомляет сильнее,
        // чем стояние, и именно это отличает обошедшего полкарты от того,
        // кто простоял у куста.
        if (paceBeat(tick, goblin.id, worldProperties.goblinPace)) {
            tireBy(goblin.tired->fatigue, kFatigueStep);
        }
        steps.push_back(StepIntent{static_cast<int>(g), step.x, step.y});
    }

    // --- 5. Кормёжка травой: один куст на всех, кто до него дотянулся ---
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

        // Съедобна не вся трава, а только верх куртины (edibleGrowth,
        // core/Body.hpp) — тот же закон, по которому её ест стадо. Белок
        // при этом считается от полной развитости: он сидит во всём
        // растении, а не в одном верху.
        const int edible = edibleGrowth(growthBefore);
        if (edible <= 0) {
            n = m;
            continue;
        }

        int eatenTotal = 0;
        int releasedTotal = 0;
        // Остатки бегут по кругу: выданное вычитается из обоих чисел, иначе
        // доли, посчитанные от полного наличия, в него не сложатся
        // (core/Share.hpp).
        int shareLeft = edible;
        int demandLeft = demand;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(bites[k].want, shareLeft, demandLeft);
            shareLeft -= eaten;
            demandLeft -= bites[k].want;
            if (eaten <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(bites[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(bites[k].claimant)].genome;

            feedBody(state, genome, eaten, kEnergyPerGrass);
            eatenTotal += eaten;

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
        // не может, отрастёт он или нет — решит PlantSystem на следующем
        // тике.
        plant->growth = std::max(0, plant->growth - eatenTotal);
        n = m;
    }

    // --- 5b. Сбор ягод: один куст на всех, кто до него дотянулся ---
    // Дележ тот же, что у травы и туши (core/Share.hpp), а вот последствие
    // другое: сам куст не убывает ни на тысячную. Луг объедают, ягодник
    // обирают — и он остаётся стоять, чтобы налиться снова.
    std::sort(picks.begin(), picks.end(), sortByCellThenId);
    for (std::size_t n = 0; n < picks.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < picks.size() && picks[m].cell == picks[n].cell) {
            demand += picks[m].want;
            ++m;
        }

        const entt::entity bushEntity = bushAt[picks[n].cell];
        auto* berries = registry.valid(bushEntity) ? registry.try_get<BerryComponent>(bushEntity) : nullptr;
        if (berries == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        const int berriesBefore = berries->berries;
        // Остатки бегут по кругу (core/Share.hpp). Ягоды — счёт штучный, и
        // без этого две ягоды на троих доставались бы НИКОМУ.
        int shareLeft = berriesBefore;
        int demandLeft = demand;
        for (std::size_t k = n; k < m; ++k) {
            const int share = shareOf(picks[k].want, shareLeft, demandLeft);
            shareLeft -= share;
            demandLeft -= picks[k].want;
            if (share <= 0) {
                continue;
            }
            const Goblin& picker = goblins[static_cast<std::size_t>(picks[k].claimant)];
            auto& state = *picker.state;
            const auto& genome = *picker.genome;

            // Крупицы уходят вместе с ягодами — тем же путём, каким они
            // уходят из травы в травоядное (core/Berries.hpp).
            const BerryPick got = pickBerries(*berries, share);
            const Portion picked{got.amount * kBerryMass, got.minerals};

            // В рот или в руки — решает занятие, а не отдельный признак у
            // намерения: желание и есть ответ на вопрос, чем гоблин сейчас
            // занят (см. GoblinDesireComponent). Запасающий не ест.
            if (picker.desire->current == GoblinDesire::Haul) {
                const Portion taken =
                    putInHands(*picker.hands, ResourceKind::Food, picked, bodySize(state, genome));
                // Не влезшее остаётся сорванным и падает под ноги: класть
                // ягоду обратно на куст мир не умеет, а терять вещество ему
                // нельзя. Кладётся оно кучей — той же, что у лагеря, и это
                // не поблажка: рассыпанное у ягодника тоже кто-нибудь
                // подберёт.
                const Portion spilled{picked.amount - taken.amount, picked.minerals - taken.minerals};
                if (spilled.amount > 0 || spilled.minerals > 0) {
                    commands.enqueue([x = picker.x, y = picker.y, spilled](World& w) {
                        depositStore(w, x, y, ResourceKind::Food, spilled);
                    });
                }
                continue;
            }
            feedBody(state, genome, picked.amount, kEnergyPerBiomass);
            takeProtein(state, genome, picked.minerals);
        }
        n = m;
    }

    // --- 5c. Еда из кучи: один запас на всех, кто до него дошёл ---
    // Дележ тот же (core/Share.hpp). Куча от еды убывает — в отличие от
    // куста и в точности как туша: принесённое кончается.
    std::sort(scoops.begin(), scoops.end(), sortByCellThenId);
    for (std::size_t n = 0; n < scoops.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < scoops.size() && scoops[m].cell == scoops[n].cell) {
            demand += scoops[m].want;
            ++m;
        }

        const entt::entity tile = terrain[scoops[n].cell];
        auto* store =
            tile != entt::null && registry.valid(tile) ? registry.try_get<StoreComponent>(tile) : nullptr;
        if (store == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        const int foodBefore = store->stored.of(ResourceKind::Food);
        // Остатки бегут по кругу (core/Share.hpp).
        int shareLeft = foodBefore;
        int demandLeft = demand;
        for (std::size_t k = n; k < m; ++k) {
            const int share = shareOf(scoops[k].want, shareLeft, demandLeft);
            shareLeft -= share;
            demandLeft -= scoops[k].want;
            if (share <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(scoops[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(scoops[k].claimant)].genome;

            const Portion got = takeFromStore(*store, ResourceKind::Food, share);
            feedBody(state, genome, got.amount, kEnergyPerBiomass);
            takeProtein(state, genome, got.minerals);
        }
        n = m;
    }

    // --- 5d. Добыча материала: одно растение на всех, кто до него дотянулся ---
    // Дележ тот же (core/Share.hpp). Растение от этого убывает — ветку
    // ломают, траву срезают, — но крупицы остаются в нём: минералы сидят в
    // корнях, а не в ветке, и когда растение умрёт, лягут перегноем целиком.
    std::sort(harvests.begin(), harvests.end(), sortByCellThenId);
    for (std::size_t n = 0; n < harvests.size();) {
        std::size_t m = n;
        int demand = 0;
        while (m < harvests.size() && harvests[m].cell == harvests[n].cell) {
            demand += harvests[m].want;
            ++m;
        }

        // Дерево или трава — решает то, что на клетке стоит: ветки берут с
        // дерева, солому с травы, и одна клетка даёт что-то одно.
        const bool fromTree = tiles.treeEntity[harvests[n].cell] != entt::null;
        const entt::entity plantEntity =
            fromTree ? tiles.treeEntity[harvests[n].cell] : plantAt[harvests[n].cell];
        auto* plant = registry.valid(plantEntity) ? registry.try_get<PlantComponent>(plantEntity) : nullptr;
        if (plant == nullptr || demand <= 0) {
            n = m;
            continue;
        }

        // Ниже kHarvestMinGrowth не обдирают: с ободранного до нуля куста
        // нечего будет взять и в следующий раз, а лагерю жить здесь долго.
        const int available = std::max(0, plant->growth - kHarvestMinGrowth);
        // Остатки бегут по кругу (core/Share.hpp).
        int shareLeft = available;
        int demandLeft = demand;
        for (std::size_t k = n; k < m; ++k) {
            const int share = shareOf(harvests[k].want, shareLeft, demandLeft);
            shareLeft -= share;
            demandLeft -= harvests[k].want;
            if (share <= 0) {
                continue;
            }
            const Goblin& worker = goblins[static_cast<std::size_t>(harvests[k].claimant)];
            const int size = bodySize(*worker.state, *worker.genome);
            const ResourceKind kind = fromTree ? ResourceKind::Twigs : ResourceKind::Straw;
            const Portion taken = putInHands(*worker.hands, kind, Portion{share, 0}, size);
            plant->growth = std::max(0, plant->growth - taken.amount);
        }
        n = m;
    }

    // --- 5e. Труд: единицы работы, вложенные в постройки ---
    // Каждый вложивший даёт по единице за тик, и кладут они в одну и ту же
    // постройку — отсюда и получается, что двое строят вдвое быстрее
    // (02_CorePrinciples.md, п.11). Ничего про "нескольких исполнителей"
    // писать отдельно не пришлось.
    //
    // Материал берётся сперва из кучи на клетке и только потом из рук
    // работающего (core/Build.hpp): принёс и сложил — достроит кто угодно.
    for (const auto& work : works) {
        const std::size_t cell = index(work.x, work.y);
        const entt::entity tile = terrain[cell];
        if (tile == entt::null || !registry.valid(tile)) {
            continue;
        }
        const auto s = static_cast<std::size_t>(work.goblin);
        if (!alive[s]) {
            continue;
        }

        auto* building = registry.try_get<BuildingComponent>(tile);
        auto* site = registry.try_get<SiteComponent>(tile);
        const BuildKind kind =
            unfinishedAt(building != nullptr ? *building : BuildingComponent{},
                          site != nullptr ? site->kind : BuildKind::None);
        if (kind == BuildKind::None) {
            continue;
        }

        auto* heap = registry.try_get<StoreComponent>(tile);
        auto& hands = goblins[s].hands->carried;

        if (building == nullptr) {
            // Первая единица труда рождает постройку — сразу с прочностью, а
            // не пустой: пустая исчезла бы от ветшания раньше, чем в неё
            // вложат вторую (см. HydrologySystem). Потому и создание, и работа
            // идут одной командой.
            BuildingComponent fresh;
            if (!applyWork(kind, fresh, heap != nullptr ? &heap->stored : nullptr, &hands)) {
                continue;
            }
            commands.enqueue([tile, fresh, kind](World& w) {
                if (!w.registry().valid(tile)) {
                    return;
                }
                if (auto* existing = w.registry().try_get<BuildingComponent>(tile)) {
                    // Пока команда ждала очереди, постройку мог создать
                    // сосед: тогда вкладываем прочность в неё, а не заводим
                    // вторую.
                    int& condition = buildingCondition(*existing, kind);
                    condition = std::min(kFull, condition + conditionPerWork(kind));
                } else {
                    w.registry().emplace<BuildingComponent>(tile, fresh);
                }
                // Замысел исполнен началом: дальше на клетке стоит не план, а
                // недостроенное здание (SiteComponent).
                if (w.registry().all_of<SiteComponent>(tile)) {
                    w.registry().remove<SiteComponent>(tile);
                }
            });
            continue;
        }

        if (!applyWork(kind, *building, heap != nullptr ? &heap->stored : nullptr, &hands)) {
            continue;
        }
        // Площадка снимается ПЕРВОЙ ЖЕ работой, а не по готовности: с этого
        // мига здесь не замысел, а недостроенное здание.
        if (site != nullptr) {
            commands.enqueue([tile](World& w) {
                if (w.registry().valid(tile) && w.registry().all_of<SiteComponent>(tile)) {
                    w.registry().remove<SiteComponent>(tile);
                }
            });
        }
    }

    // --- 6. Кормёжка падалью: одна туша на всех, кто до неё добрался ---
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
        // Остатки бегут по кругу (core/Share.hpp).
        int shareLeft = meatBefore;
        int demandLeft = demand;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(meals[k].want, shareLeft, demandLeft);
            shareLeft -= eaten;
            demandLeft -= meals[k].want;
            if (eaten <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(meals[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(meals[k].claimant)].genome;

            feedBody(state, genome, eaten, kEnergyPerBiomass);

            const int meatNow = carcass->meat;
            carcass->meat = std::max(0, carcass->meat - eaten);
            takeProtein(state, genome, releaseCarcassProtein(*carcass, meatNow, meatNow - carcass->meat));
        }
        n = m;
    }

    // --- 7. Водопой: так же долями ---
    // Клетку при этом не вычерпывают: река живёт по своему закону
    // (HydrologySystem), и водопой в этот баланс не входит.
    std::sort(drinks.begin(), drinks.end(), sortByCellThenId);
    for (std::size_t n = 0; n < drinks.size();) {
        std::size_t m = n;
        while (m < drinks.size() && drinks[m].cell == drinks[n].cell) {
            ++m;
        }
        const entt::entity tile = terrain[drinks[n].cell];
        auto* water = tile != entt::null && registry.valid(tile) ? registry.try_get<WaterComponent>(tile) : nullptr;
        if (water == nullptr) {
            n = m;
            continue;
        }
        for (std::size_t k = n; k < m; ++k) {
            auto& state = *goblins[static_cast<std::size_t>(drinks[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(drinks[k].claimant)].genome;
            state.water = std::min(waterCapacityOf(genome), state.water + drinks[k].want);
        }
        n = m;
    }

    // --- 7b. Отмахнуться ---
    // Удары этого тика разрешаются разом, одним законом и без ролей
    // (core/Strike.hpp) — тем же, каким бьёт хищник в своей системе. Ролей
    // нет и здесь: гоблин не "отвечает" зверю, он бьёт того, кто рядом, и
    // повод у него свой.
    //
    // Урон складывается: двое отмахнувшихся от одного зверя валят его вдвое
    // быстрее, и порядок обхода на итог не влияет.
    //
    // Смерть зверя здесь не разрешается, в отличие от AnimalSystem, и это то
    // же правило, что и с гоблином, только в другую сторону: хоронит тот, чья
    // система ведёт тело. Забитый насмерть зверь доживёт до следующего тика с
    // вышедшим здоровьем, и своя система его похоронит (bodyDied). Тик — цена
    // того, чтобы туша не легла дважды.
    //
    // Бьют все, кто был жив НА НАЧАЛО прохода, и бьют по тем, кто был жив на
    // начало прохода: проверяй здоровье внутри цикла — и второй удар по уже
    // добитому зверю то приходился бы, то нет, смотря чьё намерение легло в
    // список раньше. Порядок в памяти не может быть причиной события в мире
    // (02_CorePrinciples.md, п.12a).
    std::vector<bool> beastAlive(beasts.size(), false);
    for (std::size_t b = 0; b < beasts.size(); ++b) {
        beastAlive[b] = beasts[b].state->health > 0;
    }
    for (const auto& blow : blows) {
        const auto g = static_cast<std::size_t>(blow.goblin);
        const auto b = static_cast<std::size_t>(blow.beast);
        if (!alive[g] || !beastAlive[b]) {
            continue;
        }
        const StrikeOutcome outcome = resolveStrike(
            bodySize(*goblins[g].state, *goblins[g].genome), beasts[b].size, goblins[g].genome->hitChance,
            worldProperties.goblinPace, goblinSeed, tick, goblins[g].id, beasts[b].id);
        if (outcome.damage <= 0) {
            continue; // промах
        }
        beasts[b].state->health -= outcome.damage;
        applyLameness(*beasts[b].injury, outcome);
    }

    // --- 8. Шаги ---
    // Собранные намерения исполняются разом, после всех решений: иначе
    // сдвинувшийся гоблин менял бы обстановку тем, кто решает после него.
    for (const auto& step : steps) {
        const auto s = static_cast<std::size_t>(step.goblin);
        if (!alive[s]) {
            continue;
        }
        world.moveTo(goblins[s].entity, step.x, step.y);

        // Нога умяла землю там, КУДА встали (core/Trample.hpp). Здесь, а не
        // при выборе шага: намерение шагнуть — ещё не шаг, а следа не
        // оставляет тот, кто до клетки не дошёл.
        //
        // Порядок исполнения намерений на итог не влияет: сложение с
        // потолком коммутативно, и двое, ступившие на одну клетку в один
        // тик, дают одно и то же в любом порядке (04_WorldModel.md, п.8).
        if (!worldProperties.toggles.trampling) {
            continue;
        }
        const entt::entity tile = terrain[index(step.x, step.y)];
        if (tile == entt::null || !registry.valid(tile)) {
            continue;
        }
        if (auto* soil = registry.try_get<SoilComponent>(tile)) {
            trampleBy(soil->trampled, bodySize(*goblins[s].state, *goblins[s].genome));
        }
    }

    // --- 9. Встречи: кто с кем сошёлся ---
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
        if (paired[n] || !alive[static_cast<std::size_t>(matings[n].goblin)]) {
            continue;
        }
        std::size_t partner = matings.size();
        for (std::size_t k = n + 1; k < matings.size() && matings[k].cell == matings[n].cell; ++k) {
            if (paired[k] || !alive[static_cast<std::size_t>(matings[k].goblin)] ||
                matings[k].tribe != matings[n].tribe || matings[k].sex == matings[n].sex) {
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
        const Goblin& mother =
            goblins[static_cast<std::size_t>(firstIsMother ? matings[n].goblin : matings[partner].goblin)];
        const Goblin& father =
            goblins[static_cast<std::size_t>(firstIsMother ? matings[partner].goblin : matings[n].goblin)];

        const auto& motherGenome = *mother.genome;
        const auto& fatherGenome = *father.genome;
        std::uint64_t random = mixSeed(goblinSeed, mixSeed(tick, mixSeed(mother.id, father.id)));

        const auto& archetypes = tribes.tribes;
        const auto& archetype =
            (motherGenome.species >= 0 && static_cast<std::size_t>(motherGenome.species) < archetypes.size())
                ? archetypes[static_cast<std::size_t>(motherGenome.species)]
                : motherGenome;
        // Нрав племени — тем же номером и с той же оговоркой на случай, если
        // список племён почему-то короче: длину проверяем, а не полагаемся на
        // неё (см. GoblinTribesComponent).
        const auto& natures = tribes.characters;
        const CharacterComponent& natureArchetype =
            (motherGenome.species >= 0 && static_cast<std::size_t>(motherGenome.species) < natures.size())
                ? natures[static_cast<std::size_t>(motherGenome.species)]
                : *mother.nature;
        // Скрещивание — общий закон (core/generation/AnimalGenetics.hpp), а
        // таблица черт своя: она и есть вся разница между гоблином и зверем
        // в наследовании.
        const AnimalGenomeComponent childGenome =
            crossGenomes(goblinTraits(), motherGenome, fatherGenome, archetype, mutationRate, random);
        // Нрав наследуется своим законом, не бюджетом преимуществ: среднее
        // родителей, дрейф — и назад в полосу племени
        // (core/generation/GoblinCharacters.hpp).
        const CharacterComponent childNature =
            crossCharacters(*mother.nature, *father.nature, natureArchetype,
                            worldProperties.goblinCharacterSpread, random);

        AnimalComponent child;
        child.growth = kNewbornGrowth;
        child.sex = randomBelow(random, 2) == 0 ? Sex::Female : Sex::Male;

        // Ребёнок появляется не из ниоткуда: и запасы, и белок — материнские.
        // Ровно тот же обмен, что у растения с семенем и у зверя с
        // детёнышем. Отец не платит ничего.
        const int givenEnergy = mother.state->energy * kBirthEnergyShare / kFull;
        mother.state->energy -= givenEnergy;
        child.energy = std::min(givenEnergy, energyCapacityOf(childGenome));

        const int givenWater = mother.state->water * kBirthWaterShare / kFull;
        mother.state->water -= givenWater;
        child.water = std::min(givenWater, waterCapacityOf(childGenome));

        // Белок ребёнку — весь, сколько нужно ему на полный рост, и отдых
        // матери долей её жизни. Оба закона общие со зверем (AnimalSystem,
        // п.10): тело у них одно, значит и цена потомства одна.
        const int givenProtein = std::min(mother.state->protein, proteinNeedOf(childGenome));
        mother.state->protein -= givenProtein;
        child.protein = givenProtein;
        child.growth = std::min(child.growth, child.protein * kFull / proteinNeedOf(childGenome));
        mother.state->recovery = birthRestOf(*mother.genome, worldProperties.goblinPace);

        mother.desire->mating = 0;
        father.desire->mating = 0;
        mother.desire->current = GoblinDesire::Idle;
        father.desire->current = GoblinDesire::Idle;

        const std::uint64_t childId = mixSeed(random, mixSeed(mother.id, tick));
        commands.enqueue([child, childGenome, childNature, childId, x = mother.x,
                          y = mother.y](World& w) {
            const auto entity = w.registry().create();
            w.registry().emplace<IdentityComponent>(entity, IdentityComponent{childId});
            w.registry().emplace<AnimalComponent>(entity, child);
            w.registry().emplace<AnimalGenomeComponent>(entity, childGenome);
            // Новорождённый ничего ещё не хочет — тело у него полное; чего
            // хотеть, ему скажет первый же тик. И ногами он пока ничего не
            // помнит: ни шага, ни преграды.
            w.registry().emplace<GoblinDesireComponent>(entity, GoblinDesireComponent{});
            w.registry().emplace<MovementComponent>(entity);
            w.registry().emplace<GoblinComponent>(entity);
            // Цел: новорождённый ещё ни с кем не дрался. Обязателен по той же
            // причине, что и силы: обе системы выбирают существ перечнем
            // компонентов, и ребёнок без увечья выпал бы из мира молча.
            w.registry().emplace<InjuryComponent>(entity);
            // Силы полны: новорождённый ещё никуда не ходил.
            w.registry().emplace<FatigueComponent>(entity);
            // Голова пустая: родившийся не помнит ничего и узнаёт мир сам.
            // Наследовать память было бы наследованием опыта — а он берётся
            // жизнью, не рождением (02_CorePrinciples.md, п.6).
            w.registry().emplace<KnowledgeComponent>(entity);
            // Руки пусты: новорождённый ничего не несёт.
            w.registry().emplace<CarriedComponent>(entity);
            // Нрав — единственное, что ребёнок получает от родителей помимо
            // тела: он не опыт, а то, с чем рождаются.
            w.registry().emplace<CharacterComponent>(entity, childNature);
            // Знакомых нет ни одного, включая собственную мать. Наследовать
            // связи было бы наследованием чужих отношений, а они берутся
            // встречами, не рождением. Мать он узнает так же, как всех: она
            // рядом, и первый его разговор будет с нею.
            w.registry().emplace<BondsComponent>(entity);
            // Проверять клетку не нужно: существо не занимает тайл
            // (04_WorldModel.md, п.4), поэтому ребёнок всегда помещается
            // рядом с матерью.
            w.place(entity, x, y);
        });
    }

    // --- 10. Разговоры: кто кому что сказал ---
    // После шагов и встреч, но по положениям НАЧАЛА тика — тем же, по которым
    // принималось решение подойти. Иначе разговор срывался бы оттого, что
    // собеседник в этот же тик сделал шаг в сторону, а решение подойти было
    // принято, когда он стоял рядом.
    //
    // Порядок — по имени заговорившего, а не по порядку намерений: тот
    // зависит от обхода хранилища (02_CorePrinciples.md, п.12a).
    std::sort(talks.begin(), talks.end(),
              [](const TalkIntent& a, const TalkIntent& b) { return a.speakerId < b.speakerId; });
    // Разговор занимает ОБОИХ, и потому отмечаются оба: болтун, обошедший за
    // тик троих соседей, раздал бы втрое больше тепла, чем получил, а
    // окликнутый успевал бы ответить каждому, ничего на это не потратив.
    std::vector<bool> spoken(goblins.size(), false);
    for (const auto& talk : talks) {
        const auto speakerAt = static_cast<std::size_t>(talk.speaker);
        if (!alive[speakerAt] || spoken[speakerAt]) {
            continue;
        }
        std::size_t listenerAt = goblins.size();
        for (std::size_t b = 0; b < goblins.size(); ++b) {
            if (goblins[b].id == talk.listenerId) {
                listenerAt = b;
                break;
            }
        }
        if (listenerAt == goblins.size() || listenerAt == speakerAt || !alive[listenerAt] ||
            spoken[listenerAt]) {
            continue;
        }
        spoken[speakerAt] = true;
        spoken[listenerAt] = true;

        const Goblin& speaker = goblins[speakerAt];
        const Goblin& listener = goblins[listenerAt];
        std::uint64_t random = mixSeed(goblinSeed, mixSeed(tick, mixSeed(speaker.id, listener.id)));

        // Всё, что говорящий решает, он решает по тому, кем окликнутый был ДО
        // этого разговора: рассказывают и делятся по прежнему знакомству, а не
        // по тому, каким оно станет мгновением позже. Оттого и снимается здесь,
        // до всякого потепления, — и значениями, а не указателями: warmTo ниже
        // перекладывает знакомства в голове, и всякий указатель в неё после
        // этого врёт.
        const int closeness = warmthTo(*speaker.bonds, listener.id);
        const Topic topic = chooseTopic(*speaker.nature, *speaker.mind, *speaker.bonds, speaker.x, speaker.y,
                                         closeness, mind, random, sights);
        const PlaceKind toldKind = placeOf(topic);
        KnownPlace toldPlace{};
        bool told = false;
        if (toldKind != PlaceKind::None) {
            if (const KnownPlace* known = recall(*speaker.mind, toldKind, speaker.x, speaker.y)) {
                toldPlace = *known;
                told = true;
            }
        }
        std::uint64_t praisedId = 0;
        if (topic == Topic::Kin) {
            if (const Acquaintance* praised = closestFace(*speaker.bonds)) {
                praisedId = praised->id;
            }
        }

        // Тепло — обоим, и каждому по обаянию ДРУГОГО: обаяние есть то, что
        // существо оставляет, а не то, что чувствует (core/Bonds.hpp).
        warmTo(*listener.bonds, speaker.id, warmthFrom(speaker.nature->charming));
        warmTo(*speaker.bonds, listener.id, warmthFrom(listener.nature->charming));

        if (told) {
            // Слух ложится тем же вызовом, что и своё воспоминание, — и в этом
            // весь расчёт: правило вытеснения (core/Knowledge.hpp) само не даст
            // чужому слову выбить из головы место, на котором гоблин бывал.
            remember(*listener.mind, toldPlace.kind, toldPlace.x, toldPlace.y,
                     hearsayGain(interestIn(*listener.nature, topic)));
        } else if (praisedId != 0 && praisedId != listener.id) {
            // Доброе слово о третьем: слушатель теплеет к тому, кого ещё не
            // встречал. Так тепло уходит дальше пары говорящих.
            warmTo(*listener.bonds, praisedId, kGoodWord);
        }

        // И ноша — но только между своими, и только у того, у кого она есть.
        // Порог выше, чем у вестей, намеренно: сказать, где ягодник, дешевле,
        // чем отдать горсть ягод, и порядок знакомства этим и держится.
        //
        // Половина того, что в руках, а не всё: делятся, а не отдают. Больше
        // руки окликнутого всё равно не примут (carryRoom, core/Carry.hpp),
        // поэтому берут сразу столько, сколько влезет: остаток иначе пропал бы
        // между двумя вызовами.
        if (closeness >= kShareWarmth) {
            const int listenerSize = bodySize(*listener.state, *listener.genome);
            const int inHands = speaker.hands->carried.of(ResourceKind::Food);
            const int given = std::min(inHands / 2, carryRoom(*listener.hands, listenerSize));
            if (given > 0) {
                const Portion shared = takeFromHands(*speaker.hands, ResourceKind::Food, given);
                putInHands(*listener.hands, ResourceKind::Food, shared, listenerSize);
            }
        }

        // Тоска утолена у обоих: окликнутому тоже больше не с чего скучать.
        speaker.desire->talking = 0;
        listener.desire->talking = 0;
        // А вот занятие меняется только у заговорившего. Окликнутому его не
        // сбрасывают намеренно: он разговора не выбирал, и терять из-за него
        // начатое дело значило бы позволить болтуну сбивать с работы всё
        // поселение.
        speaker.desire->current = GoblinDesire::Idle;
    }
}

// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
// Того, что общее со зверем, здесь нет: оно перечислено в группе животных,
// потому что живёт в core/Body.hpp и одинаково для обоих.
void appendGoblinSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Goblins (tick)";
    out.push_back({g, "kDesireFloor", kDesireFloor});
    out.push_back({g, "kDesireSwitch", kDesireSwitch});
    out.push_back({g, "kPanic", kPanic});
    out.push_back({g, "kBreedingGrowth", kBreedingGrowth});
    out.push_back({g, "kCalmNeed", kCalmNeed});
    out.push_back({g, "kMateDesire", kMateDesire});
    out.push_back({g, "kWanderChance", kWanderChance});
    out.push_back({g, "kBerryPick", kBerryPick});
    out.push_back({g, "kHaulUrge", kHaulUrge});

    // Ноша (core/Carry.hpp) — своей парой: ёмкость рук и цена шага с ними
    // подбираются друг против друга.
    constexpr const char* c = "Goblins (carry)";
    out.push_back({c, "kCarryPerSize", static_cast<float>(kCarryPerSize)});
    out.push_back({c, "kCarryStepCost", static_cast<float>(kCarryStepCost)});

    // Стройка (core/Build.hpp, core/Work.hpp) — своей группой: цена труда,
    // цена материала и скорость ветшания подбираются друг против друга.
    constexpr const char* b = "Goblins (build)";
    out.push_back({b, "kBuildUrge", kBuildUrge});
    out.push_back({b, "kWorkBedding", static_cast<float>(kWorkBedding)});
    out.push_back({b, "kWorkCanopy", static_cast<float>(kWorkCanopy)});
    out.push_back({b, "kBuildDecayPeriod", static_cast<float>(kBuildDecayPeriod)});
    out.push_back({b, "kMaterialPerWork", static_cast<float>(kMaterialPerWork)});
    out.push_back({b, "kTwigStrength", static_cast<float>(kTwigStrength)});
    out.push_back({b, "kStrawHarvest", static_cast<float>(kStrawHarvest)});
    out.push_back({b, "kTwigHarvest", static_cast<float>(kTwigHarvest)});
    out.push_back({b, "kHarvestMinGrowth", static_cast<float>(kHarvestMinGrowth)});
    out.push_back({g, "kRoamTicks", static_cast<float>(kRoamTicks)});
    out.push_back({g, "kHomeRange", static_cast<float>(kHomeRange)});
    // Годность места для отдыха — свои слагаемые (core/Rest.hpp): их
    // подбирают вместе, друг против друга, и смотреть на них надо рядом.
    constexpr const char* r = "Goblins (rest)";
    out.push_back({r, "kRestBase", kRestBase});
    out.push_back({r, "kRestDryWeight", kRestDryWeight});
    out.push_back({r, "kRestShelter", kRestShelter});
    out.push_back({r, "kRestRockPenalty", kRestRockPenalty});
    out.push_back({r, "kRestCarcassPenalty", kRestCarcassPenalty});
    out.push_back({r, "kRestTrodden", kRestTrodden});
    out.push_back({r, "kRestGood", kRestGood});

    // Память места (core/Knowledge.hpp) — своей группой: эти числа решают,
    // насколько твёрдо гоблин держится за уже известное.
    constexpr const char* m = "Goblins (memory)";
    out.push_back({m, "kKnownPlaces", static_cast<float>(kKnownPlaces)});
    out.push_back({m, "kRememberGain", kRememberGain});
    out.push_back({m, "kForgetRate", kForgetRate});
    out.push_back({m, "kDisappointLoss", kDisappointLoss});
    out.push_back({m, "kRecallDistance", kRecallDistance});
    out.push_back({m, "kRestReturn", kRestReturn});
    out.push_back({m, "kMetMark", kMetMark});
    out.push_back({m, "kMateMark", kMateMark});

    // Страх (core/Fear.hpp) — своей группой, и в ней два числа не про сам
    // страх, а про то, что от него остаётся: твёрдость испуга в памяти и
    // цена шага в сторону опасного. Подбираются они друг против друга —
    // испуг, который не запомнился, сторониться нечему.
    //
    // kCarcassFearWeight здесь нет намеренно: гоблин падали не боится (её
    // место уже отнимает годность, kRestCarcassPenalty), и показывать число,
    // которого эта система не читает, значило бы врать наблюдателю.
    constexpr const char* s = "Goblins (fear)";
    out.push_back({s, "kWoundFear", kWoundFear});
    out.push_back({s, "kScareMark", kScareMark});
    out.push_back({s, "kDangerShy", kDangerShy});

    // Нрав (core/Character.hpp) — два размаха, и смотреть на них надо рядом:
    // ими и разделены уровень (трудолюбие) и направление (склонность).
    constexpr const char* h = "Goblins (character)";
    out.push_back({h, "kDiligenceSwing", kDiligenceSwing});
    out.push_back({h, "kInterestSwing", kInterestSwing});
    out.push_back({h, "kTribeSpread", kTribeSpread});
    out.push_back({h, "kTribeMark", kTribeMark});
    out.push_back({h, "kCharacterDrift", kCharacterDrift});

    // Связи (core/Bonds.hpp) — прибавка, остывание и два порога знакомства на
    // одной шкале: подбираются друг против друга и порознь не значат ничего.
    constexpr const char* f = "Goblins (bonds)";
    out.push_back({f, "kKnownFaces", static_cast<float>(kKnownFaces)});
    out.push_back({f, "kWarmGain", kWarmGain});
    out.push_back({f, "kCharmSwing", kCharmSwing});
    out.push_back({f, "kCoolPeriod", static_cast<float>(kCoolPeriod)});
    out.push_back({f, "kNewsWarmth", kNewsWarmth});
    out.push_back({f, "kShareWarmth", kShareWarmth});

    // Разговор (core/Talk.hpp): к кому подойти и что от этого останется.
    constexpr const char* t = "Goblins (talk)";
    out.push_back({t, "kTalkRange", static_cast<float>(kTalkRange)});
    out.push_back({t, "kTalkNear", kTalkNear});
    out.push_back({t, "kTalkDistance", kTalkDistance});
    out.push_back({t, "kTalkNovelty", kTalkNovelty});
    out.push_back({t, "kStrangerTribe", kStrangerTribe});
    out.push_back({t, "kHearsayFull", kHearsayFull});
    out.push_back({t, "kGoodWord", kGoodWord});
    out.push_back({f, "kMateWarmthStep", kMateWarmthStep});
}

} // namespace goblins

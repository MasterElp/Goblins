#include "core/systems/GoblinSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "core/Body.hpp"
#include "core/Berries.hpp"
#include "core/Build.hpp"
#include "core/Carry.hpp"
#include "core/Carcass.hpp"
#include "core/Desires.hpp"
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
#include "core/Resources.hpp"
#include "core/Store.hpp"
#include "core/Work.hpp"
#include "core/TileSnapshot.hpp"
#include "core/Trample.hpp"
#include "core/Walk.hpp"
#include "core/components/AnimalComponent.hpp"
#include "core/components/AnimalGenomeComponent.hpp"
#include "core/components/BerryComponent.hpp"
#include "core/components/BuildingComponent.hpp"
#include "core/components/CarriedComponent.hpp"
#include "core/components/CarcassComponent.hpp"
#include "core/components/GoblinComponent.hpp"
#include "core/components/GoblinDesireComponent.hpp"
#include "core/components/GoblinTribesComponent.hpp"
#include "core/components/IdentityComponent.hpp"
#include "core/components/KnowledgeComponent.hpp"
#include "core/components/MovementComponent.hpp"
#include "core/components/PlantComponent.hpp"
#include "core/components/PositionComponent.hpp"
#include "core/components/SoilComponent.hpp"
#include "core/components/StoreComponent.hpp"
#include "core/components/SiteComponent.hpp"
#include "core/components/TimeComponent.hpp"
#include "core/components/WaterComponent.hpp"
#include "core/components/WorldPropertiesComponent.hpp"
#include "core/generation/AnimalGenetics.hpp"
#include "core/generation/GoblinGenetics.hpp"

namespace goblins {

namespace {

// Пороги желаний. Числа те же, что у животных, но константы свои, и это не
// оплошность: у гоблина скоро появятся желания, которых у зверя нет и быть
// не может (отдых, ноша, работа), и равновесие между ними придётся крутить
// отдельно. Общим здесь остаётся закон (core/Desires.hpp), а не значения.
constexpr int kDesireFloor = 350;
constexpr int kDesireSwitch = 150;

// Продолжение рода. Желание копится только у взрослого, доросшего и не
// бедствующего (kCalmNeed — предел голода и жажды, при котором ещё не
// бедствие), а идти искать пару гоблин начинает с kMateDesire.
constexpr int kBreedingGrowth = 900;
constexpr int kCalmNeed = 750;
constexpr int kMateDesire = 600;

// Усталость: сколько её прибывает за тик просто оттого, что гоблин жив, и
// сколько сверх того стоит сделанный шаг.
//
// Целыми числами за тик, без накопителя (core/Scale.hpp). Ходьба дороже
// стояния втрое — иначе усталость была бы просто вторым возрастом и не
// значила бы ничего: она должна отличать того, кто обошёл полкарты, от
// того, кто простоял у куста.
//
// Размером тела не делится, в отличие от расхода энергии: маленький устаёт
// не меньше взрослого, а скорее больше. Делать из этого черту генома было
// бы преждевременно — сперва надо увидеть, что усталость вообще делает с
// поведением.
constexpr int kFatigueTick = 1;
constexpr int kFatigueStep = 3;

// Сколько усталости уходит за тик отдыха. Заметно больше, чем прибывает:
// отдых должен занимать меньшую часть жизни, чем дорога, иначе поселение
// будет состоять из лежащих.
//
// Прибывает при этом и во время отдыха (kFatigueTick вычитается из этого
// числа, а не отменяется): гоблин отдыхает, но не перестаёт жить.
constexpr int kRestRelief = 8;

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
    // Своё, гоблинское: усталость. Тело (AnimalComponent) у него общее со
    // зверем, а это — нет.
    GoblinComponent* own = nullptr;
    // Память мест. Единственное, чего у зверя нет вовсе.
    KnowledgeComponent* mind = nullptr;
    // Руки. Общее для всего живого (core/Carry.hpp), просто носит пока
    // только гоблин.
    CarriedComponent* hands = nullptr;

    // Голод и жажда живут здесь, в снимке тика, а не в компоненте: оба
    // пересчитываются из тела заново каждый тик (core/Needs.hpp), и
    // пережить тик им незачем.
    int hunger = 0;
    int thirst = 0;
};

// Какое желание сейчас гонит гоблина. Сам выбор — общий закон мира
// (core/Desires.hpp); здесь только то, чего гоблин может хотеть и чем
// меряется срочность каждого.
//
// Страха в этом списке нет, и это не забывчивость: бояться гоблину пока
// некого — хищник его не видит (см. GoblinSystem.hpp). Появится опасность —
// появится и желание, и встанет оно последним, чтобы побеждать при
// равенстве.
GoblinDesire chooseGoblinDesire(const Goblin& goblin, bool readyToMate, bool hasHome, int building) {
    const GoblinDesireComponent& desire = *goblin.desire;
    const int mating = readyToMate && desire.mating >= kMateDesire ? desire.mating : 0;
    // Запасать некуда — незачем и начинать. Гейт стоит здесь, а не в самой
    // ветке: желание, которое нельзя исполнить, не должно даже побеждать
    // (иначе гоблин "занят" тем, чего не делает).
    const int hauling = hasHome ? kHaulUrge : 0;

    // Порядок — приоритет при равенстве, побеждает последний. Отдых стоит
    // первым и потому проигрывает всем: усталость никого не убивает, а
    // голод и жажда убивают. Лечь гоблин должен тогда, когда его больше
    // ничто не гонит, — и это не поблажка, а точное описание того, чем
    // отдых отличается от еды.
    const Urgency candidates[] = {
        {static_cast<int>(GoblinDesire::Rest), goblin.own->fatigue},
        // Запасание — сразу после отдыха и раньше всего остального в списке,
        // то есть проигрывает и голоду, и жажде, и паре: набирать впрок имеет
        // смысл только сытым.
        {static_cast<int>(GoblinDesire::Haul), hauling},
        // Стройка — после запаса, то есть при равенстве побеждает она: запас
        // делается впрок и подождёт, а стройка — ответ на конкретную нехватку
        // здесь и сейчас. Голоду и жажде она всё равно проигрывает.
        {static_cast<int>(GoblinDesire::Build), building},
        {static_cast<int>(GoblinDesire::Food), goblin.hunger},
        {static_cast<int>(GoblinDesire::Water), goblin.thirst},
        {static_cast<int>(GoblinDesire::Mate), mating},
    };

    int currentUrgency = 0;
    switch (desire.current) {
        case GoblinDesire::Food: currentUrgency = goblin.hunger; break;
        case GoblinDesire::Water: currentUrgency = goblin.thirst; break;
        case GoblinDesire::Mate: currentUrgency = mating; break;
        case GoblinDesire::Rest: currentUrgency = goblin.own->fatigue; break;
        case GoblinDesire::Haul: currentUrgency = hauling; break;
        case GoblinDesire::Build: currentUrgency = building; break;
        case GoblinDesire::Idle: break;
    }

    return static_cast<GoblinDesire>(chooseUrgent(candidates, static_cast<int>(desire.current), currentUrgency,
                                                   kDesireFloor, kDesireSwitch,
                                                   static_cast<int>(GoblinDesire::Idle)));
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
    auto goblinView =
        registry.view<AnimalComponent, AnimalGenomeComponent, GoblinDesireComponent, IdentityComponent,
                       MovementComponent, PositionComponent, GoblinComponent, KnowledgeComponent,
                       CarriedComponent>();
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
                                  &goblinView.get<GoblinComponent>(entity),
                                  &goblinView.get<KnowledgeComponent>(entity),
                                  &goblinView.get<CarriedComponent>(entity)});
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

    // --- 3. Тело и желания ---
    // Отдельным проходом от решений (п.4) намеренно: гоблин, выбирая пару,
    // смотрит, чего хочет сосед, — и если бы желания и решения считались в
    // одном проходе, сосед, которого EnTT хранит позже, был бы ещё с
    // прошлотиковым желанием. Порядок в памяти не может быть причиной
    // события в мире (02_CorePrinciples.md, п.12a).
    std::vector<bool> alive(goblins.size(), true);
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        Goblin& goblin = goblins[g];
        auto& state = *goblin.state;
        const auto& genome = *goblin.genome;
        auto& desire = *goblin.desire;

        advanceBody(state, genome, tick, goblin.id);

        // Своих бед сверх общего закона у гоблина пока нет: болезни от
        // тесноты он не знает (поселение тесно по сути), зубов на него никто
        // не точит. Поэтому между телом и смертью здесь ничего и не стоит.
        if (bodyDied(state, genome)) {
            enqueueDeath(commands, goblin.entity, goblin.x, goblin.y);
            alive[g] = false;
            continue;
        }

        // Память ног тает со временем, а не от шагов (core/Walk.hpp):
        // простоявший сотню тиков у куста не должен помнить преграду,
        // которой давно нет.
        fadeWalkMemory(*goblin.memory);

        // Усталость прибывает от того, что гоблин жив. Шаг добавит своё
        // ниже, в фазе шагов, а отдых вычтет своё в фазе решений: и то, и
        // другое — следствия того, чем он занят, и считать их здесь, до
        // выбора занятия, было бы гаданием.
        goblin.own->fatigue = std::min(kFull, goblin.own->fatigue + kFatigueTick);

        // Память тает сама. Не изнашивание и не уборка: именно забывание и
        // заставляет возвращаться — помни гоблин вечно, ему хватило бы
        // одного обхода мира на всю жизнь (core/Knowledge.hpp).
        forget(*goblin.mind);

        goblin.hunger = hungerOf(state, genome);
        goblin.thirst = thirstOf(state, genome);

        const bool adult = state.age >= genome.maturityAge && state.growth >= kBreedingGrowth;
        const bool content = state.health >= kFull && goblin.hunger < kCalmNeed && goblin.thirst < kCalmNeed;
        if (adult && content) {
            desire.mating = std::min(kFull, desire.mating + genome.breedingUrge);
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
            const auto* home = recall(*goblin.mind, PlaceKind::Rest, goblin.x, goblin.y, kRestReturn);
            if (home != nullptr && home->x == goblin.x && home->y == goblin.y) {
                const RestPlace place{tiles.moisture[here], tiles.rockiness[here], tiles.treeAt[here] != 0,
                                       tiles.carcassMeat[here], tiles.trampled[here], tiles.canopy[here],
                                       tiles.bedding[here]};
                building = std::max(0, kRestGood - restQualityOf(place));
                // Вторая причина — куча под открытым небом. Еда портится, и
                // это видно тому, кто стоит рядом с ней. Крыша над кучей и
                // есть склад (core/Store.hpp), отдельной постройки для него
                // не нужно.
                if (tiles.canopy[here] < kFull) {
                    const int uncovered = tiles.storeFood[here] * (kFull - tiles.canopy[here]) / kFull;
                    building = std::max(building, std::min(kFull, uncovered * kFull / kStoreShelterFull));
                }
            }
            // Начатое надо доводить: незаконченный замысел держит сам по
            // себе, без всякой нехватки. Помнит гоблин свою площадку или
            // видит чужую — разницы нет, вкладываться можно во всякую.
            if (building < kBuildUrge && recall(*goblin.mind, PlaceKind::Work, goblin.x, goblin.y) != nullptr) {
                building = kBuildUrge;
            }
        }
        desire.current = chooseGoblinDesire(goblin, adult && content, hasHome, building);
    }

    // Возможная пара в том виде, в каком её видно со стороны
    // (core/Mating.hpp). Желания у всех уже посчитаны (п.3), поэтому
    // "согласен" здесь честное, а не прошлотиковое.
    //
    // Хищником не назван никто: закон встречи различает диеты, потому что у
    // животных ими различаются виды, а гоблины все одной таблицы —
    // различает их племя, и оно едет в поле species.
    std::vector<MateCandidate> mates;
    for (std::size_t g = 0; g < goblins.size(); ++g) {
        mates.push_back(MateCandidate{goblins[g].id, goblins[g].x, goblins[g].y, goblins[g].genome->species,
                                       false, goblins[g].state->sex,
                                       alive[g] && goblins[g].desire->current == GoblinDesire::Mate});
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
        const int size = bodySize(state.growth);

        // Случайность собирается из seed мира, номера тика и постоянного
        // идентификатора (core/Random.hpp). Не из координат: клетка меняется
        // каждый шаг, а на одной клетке их может стоять несколько — розыгрыш
        // вышел бы одинаковым.
        std::uint64_t random = mixSeed(goblinSeed, mixSeed(tick, goblin.id));

        const int reach = std::max(1, genome.perception);
        bool busy = false;
        bool hasTarget = false;
        int targetX = goblin.x;
        int targetY = goblin.y;

        // Куда гоблин вообще может встать (core/Path.hpp, standableAt): не
        // за границей Области, не на занятый непроходимым объектом тайл и не
        // в воду. Правило общее, здесь только факты, из которых оно
        // складывается: снимок тайлов этого тика.
        auto standable = [&](int nx, int ny) {
            if (!world.area().inBounds(nx, ny)) {
                return false;
            }
            const std::size_t cell = index(nx, ny);
            return standableAt(world.area().isBlocked(nx, ny), terrain[cell] != entt::null, waterAt[cell]);
        };

        // Ближайшая клетка в пределах видимости, удовлетворяющая условию.
        // Ближайшая, а не лучшая: гоблин идёт к тому, что видит рядом, а не
        // выбирает оптимум по всей округе. Из одинаково близких выбор
        // бросается жребием — иначе обход, идущий с левого верхнего угла,
        // уводил бы всех дружно вверх и влево.
        auto findNearest = [&](auto predicate, int& outX, int& outY) {
            int bestDistance = 0;
            int ties = 0;
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
                    if (ties > 0 && distance > bestDistance) {
                        continue;
                    }
                    if (!predicate(index(nx, ny), nx, ny)) {
                        continue;
                    }
                    if (ties > 0 && distance == bestDistance) {
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
                        takeFromHands(*goblin.hands, ResourceKind::Food, genome.biteSize * size / kFull);
                    feedBody(state, genome, bite.amount);
                    takeProtein(state, genome, bite.minerals);
                    busy = true;
                    break;
                }
                // Куча под ногами — вторая: она в известном месте и никуда не
                // денется, но за ней всё же надо было дойти.
                if (storeFood[here] > kMinBiteGrowth) {
                    scoops.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, genome.biteSize * size / kFull});
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
                        ShareIntent{here, static_cast<int>(g), goblin.id, genome.biteSize * size / kFull});
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
                if (plantAt[here] != entt::null && plantGrowth[here] > kMinBiteGrowth) {
                    bites.push_back(
                        ShareIntent{here, static_cast<int>(g), goblin.id, genome.biteSize * size / kFull});
                    busy = true;
                    break;
                }

                // Под ногами пусто — ищем глазами. Сначала падаль: она
                // редка, лежит в одной точке, и идти к ней надо дорогой
                // (core/Path.hpp), иначе увиденная через реку заведёт
                // гоблина на берег и оставит там. Волна считается только
                // тогда, когда есть на что смотреть: перебор клеток дёшев, а
                // волна по округе — нет.
                int meatX = goblin.x;
                int meatY = goblin.y;
                const bool meatSeen =
                    findNearest([&](std::size_t cell, int nx, int ny) {
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

                // Вспомненный ягодник — раньше видимой травы, и это главное
                // в диете собирателя. Трава под ногами голод перебьёт, но
                // ИДТИ за ней незачем: она везде, и ушедший за ней гоблин
                // просто перестал бы возвращаться куда бы то ни было.
                if (!hasTarget) {
                    hasTarget = goByMemory(PlaceKind::Food);
                }

                // Трава — последняя и только та, что видно рядом. Идут к ней
                // напрямик, а преграду обходят вслепую памятью ног: упираться
                // в берег ради пучка травы незачем.
                if (!hasTarget) {
                    hasTarget = findNearest(
                                    [&](std::size_t cell, int nx, int ny) {
                                        return plantAt[cell] != entt::null && plantGrowth[cell] > kMinBiteGrowth &&
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
                        ShareIntent{source, static_cast<int>(g), goblin.id, kDrinkRate * size / kFull});
                    // Помнится берег, на котором стоял, а не сама вода: в
                    // воду гоблин шагнуть не может, и место водопоя — это
                    // клетка под ногами.
                    remember(*goblin.mind, PlaceKind::Water, goblin.x, goblin.y);
                    busy = true;
                } else {
                    hasTarget = findNearest([&](std::size_t cell, int, int) { return waterAt[cell] > 0; },
                                             targetX, targetY) >= 0;
                    if (!hasTarget) {
                        hasTarget = goByMemory(PlaceKind::Water);
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
                    const MateChoice call = hearCall(suitor, mates);
                    if (call.found) {
                        targetX = call.x;
                        targetY = call.y;
                        hasTarget = true;
                    }
                    break;
                }
                reachOf.build(world.area(), goblin.x, goblin.y, reach, standable);
                const MateChoice mate = chooseMate(reachOf, suitor, mates);
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
                    goblin.own->fatigue = std::max(0, goblin.own->fatigue - kRestRelief);
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
                hasTarget = goByMemory(PlaceKind::Rest, kRestReturn);

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
                // Ничего не вспомнилось и ничего не видно — идти по слабой
                // памяти лучше, чем брести наугад: место, к которому ходили
                // мало, всё же вероятнее годного случайного.
                if (!hasTarget) {
                    hasTarget = goByMemory(PlaceKind::Rest);
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

                // Ягодника не видно — идти к нему дорогой, как за едой:
                // ягодник редок и стоит в одной точке.
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
                // Не видно — вспомнить, где еда была. С пустыми руками идти
                // домой незачем, а вот к ягоднику — затем и затевалось.
                if (!hasTarget) {
                    hasTarget = goByMemory(PlaceKind::Food);
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
                        const BuildKind kind = betterBuild(place);
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
                        break;
                    }
                    // Не видно — идти к вспомненной стройке. Придём и не
                    // найдём — там же и разочаруемся (goByMemory).
                    hasTarget = goByMemory(PlaceKind::Work);
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

        // Направление поиска, когда желаемого не видно. Гоблин идёт в одну
        // сторону целый отрезок пути (kRoamTicks), а не топчется на месте.
        // Берётся из постоянного идентификатора и номера отрезка, поэтому
        // системе не нужно ничего помнить между тиками (05_Entity.md, п.3).
        auto roamDirection = [&]() {
            std::uint64_t roam = mixSeed(goblin.id, tick / kRoamTicks);
            return static_cast<int>(nextState(roam) % 8u);
        };

        // Куда гоблин хочет — ОДНО направление на все случаи движения, и
        // дальше шаг считается одинаково (core/Walk.hpp). Идущий к цели,
        // ищущий за пределами видимости и просто бродящий отличаются только
        // тем, откуда взялось это направление; обход преграды получается
        // сам.
        int aim = -1;
        if (hasTarget) {
            aim = walkDirectionTo(goblin.x, goblin.y, targetX, targetY);
        } else if (desire.current != GoblinDesire::Idle) {
            aim = roamDirection();
        } else if (static_cast<int>(randomBelow(random, kFull)) >= kWanderChance) {
            continue; // ничего не гонит — стоит
        }

        // Сторониться гоблину пока некого: чужое племя ему не соперник за
        // траву настолько, чтобы обходить его стороной, а хищника он не
        // видит. Поэтому WalkShy пустой — но он есть, и в него встанет
        // первая же причина держаться подальше.
        // Седьмое слагаемое шага: по натоптанному идти легче
        // (core/Walk.hpp). Читается из снимка тика, как и всё остальное,
        // чтобы решения всех гоблинов принимались по одному состоянию мира.
        const auto trodden = [&](int nx, int ny) {
            return world.area().inBounds(nx, ny) ? tiles.trampled[index(nx, ny)] : 0;
        };
        const WalkStep step =
            chooseStep(*goblin.memory, goblin.x, goblin.y, aim, WalkShy{}, standable, trodden, random);
        if (!step.moved) {
            continue; // шагнуть некуда вовсе: вода, камень или край мира
        }

        // Шаг с ношей дороже пустого (core/Carry.hpp). Без этой платы носить
        // всегда было бы выгоднее, чем не носить, и решать тут было бы
        // нечего; с ней дальний ягодник окупается хуже ближнего — а решает
        // это не гоблин, а мир.
        state.energy =
            std::max(0, state.energy - carryStepEnergy(kStepEnergy * size / kFull, *goblin.hands, size));
        // Шаг стоит не только энергии, но и сил: ходьба утомляет сильнее,
        // чем стояние, и именно это отличает обошедшего полкарты от того,
        // кто простоял у куста.
        goblin.own->fatigue = std::min(kFull, goblin.own->fatigue + kFatigueStep);
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

        int eatenTotal = 0;
        int releasedTotal = 0;
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(bites[k].want, growthBefore, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(bites[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(bites[k].claimant)].genome;

            feedBody(state, genome, eaten);
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
        for (std::size_t k = n; k < m; ++k) {
            const int share = shareOf(picks[k].want, berriesBefore, demand);
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
                    putInHands(*picker.hands, ResourceKind::Food, picked, bodySize(state.growth));
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
            feedBody(state, genome, picked.amount);
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
        for (std::size_t k = n; k < m; ++k) {
            const int share = shareOf(scoops[k].want, foodBefore, demand);
            if (share <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(scoops[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(scoops[k].claimant)].genome;

            const Portion got = takeFromStore(*store, ResourceKind::Food, share);
            feedBody(state, genome, got.amount);
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
        for (std::size_t k = n; k < m; ++k) {
            const int share = shareOf(harvests[k].want, available, demand);
            if (share <= 0) {
                continue;
            }
            const Goblin& worker = goblins[static_cast<std::size_t>(harvests[k].claimant)];
            const int size = bodySize(worker.state->growth);
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
        for (std::size_t k = n; k < m; ++k) {
            const int eaten = shareOf(meals[k].want, meatBefore, demand);
            if (eaten <= 0) {
                continue;
            }
            auto& state = *goblins[static_cast<std::size_t>(meals[k].claimant)].state;
            const auto& genome = *goblins[static_cast<std::size_t>(meals[k].claimant)].genome;

            feedBody(state, genome, eaten);

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
            state.water = std::min(genome.waterCapacity, state.water + drinks[k].want);
        }
        n = m;
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
            trampleBy(soil->trampled, bodySize(goblins[s].state->growth));
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
        // Скрещивание — общий закон (core/generation/AnimalGenetics.hpp), а
        // таблица черт своя: она и есть вся разница между гоблином и зверем
        // в наследовании.
        const AnimalGenomeComponent childGenome =
            crossGenomes(goblinTraits(), motherGenome, fatherGenome, archetype, mutationRate, random);

        AnimalComponent child;
        child.growth = kNewbornGrowth;
        child.sex = randomBelow(random, 2) == 0 ? Sex::Female : Sex::Male;

        // Ребёнок появляется не из ниоткуда: и запасы, и белок — материнские.
        // Ровно тот же обмен, что у растения с семенем и у зверя с
        // детёнышем. Отец не платит ничего.
        const int givenEnergy = mother.state->energy * kBirthEnergyShare / kFull;
        mother.state->energy -= givenEnergy;
        child.energy = std::min(givenEnergy, childGenome.energyCapacity);

        const int givenWater = mother.state->water * kBirthWaterShare / kFull;
        mother.state->water -= givenWater;
        child.water = std::min(givenWater, childGenome.waterCapacity);

        const int givenProtein = std::max(0, mother.state->protein / 3);
        mother.state->protein -= givenProtein;
        child.protein = givenProtein;
        child.growth = childGenome.proteinNeed > 0
                            ? std::min(child.growth, child.protein * kFull / childGenome.proteinNeed)
                            : child.growth;

        mother.desire->mating = 0;
        father.desire->mating = 0;
        mother.desire->current = GoblinDesire::Idle;
        father.desire->current = GoblinDesire::Idle;

        const std::uint64_t childId = mixSeed(random, mixSeed(mother.id, tick));
        commands.enqueue([child, childGenome, childId, x = mother.x, y = mother.y](World& w) {
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
            // Голова пустая: родившийся не помнит ничего и узнаёт мир сам.
            // Наследовать память было бы наследованием опыта — а он берётся
            // жизнью, не рождением (02_CorePrinciples.md, п.6).
            w.registry().emplace<KnowledgeComponent>(entity);
            // Руки пусты: новорождённый ничего не несёт.
            w.registry().emplace<CarriedComponent>(entity);
            // Проверять клетку не нужно: существо не занимает тайл
            // (04_WorldModel.md, п.4), поэтому ребёнок всегда помещается
            // рядом с матерью.
            w.place(entity, x, y);
        });
    }
}

// Константы этой системы — наружу только для чтения (core/Diagnostics.hpp).
// Того, что общее со зверем, здесь нет: оно перечислено в группе животных,
// потому что живёт в core/Body.hpp и одинаково для обоих.
void appendGoblinSystemConstants(std::vector<ConstantInfo>& out) {
    constexpr const char* g = "Goblins (tick)";
    out.push_back({g, "kDesireFloor", kDesireFloor});
    out.push_back({g, "kDesireSwitch", kDesireSwitch});
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
    out.push_back({g, "kFatigueTick", kFatigueTick});
    out.push_back({g, "kFatigueStep", kFatigueStep});
    out.push_back({g, "kRestRelief", kRestRelief});

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
}

} // namespace goblins

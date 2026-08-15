#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "config/Config.hpp"
#include "world/WorldSaveInfo.hpp"

// Снимок состояния мира на стороне клиента — обновляется из сетевого
// потока (IXWebSocket), читается из потока рендера. Общий для экранов
// "Генерация мира" и "Симуляция": обе показывают один и тот же мир,
// просто по-разному его отображают.
struct WorldState {
    // Номер версии состояния: растёт на каждое применённое сообщение
    // сервера. По нему потребители, для которых пересчёт дорог (карта в
    // текстуре, см. MapTexture), понимают, что состояние действительно
    // новое, а не тот же самый снимок, отданный на следующем кадре.
    std::uint64_t version = 0;

    int areaWidth = 0;
    int areaHeight = 0;
    std::uint64_t tick = 0;
    bool paused = false;
    std::vector<std::pair<int, int>> boulders;
    // Истоки рек + "родники" (WaterSourceComponent) — тег без данных, как
    // boulders.
    std::vector<std::pair<int, int>> waterSources;

    // Почва — плоские массивы (row-major, x + y*areaWidth), одно значение
    // на тайл. Вода — так же; глубина 0 значит "воды нет" (сам компонент
    // на сервере просто отсутствует у тайла — см. 02_CorePrinciples.md,
    // п.3: отсутствие компонента = отсутствие возможности). minerals —
    // целое количество (SoilComponent.minerals), не нормализовано.
    std::vector<float> moisture;
    std::vector<float> rockiness;
    std::vector<float> compaction;
    std::vector<int> minerals;
    std::vector<float> waterDepth;

    // Высота рельефа (HeightComponent.height) — единиц измерения не несёт,
    // экраны сами нормализуют по min/max текущей карты для слоя-рельефа.
    std::vector<float> height;

    // Растения — плотные массивы, как почва: -1 в plantSpecies значит
    // "клетка пустая", иначе это индекс вида травы (см. plantSpecies
    // ниже), а plantGrowth — развитость растения, 0..1. Перегной —
    // разреженный, как вода: количество минералов, которые ещё вернутся
    // в почву (0 — перегноя нет).
    std::vector<int> plantSpeciesAt;
    std::vector<float> plantGrowth;
    std::vector<int> humus;

    // Виды травы этого мира: для каждого — набор пар "имя черты ->
    // значение", как их прислал сервер. Именно пары, а не структура с
    // полями: клиент не знает и не должен знать состав генома (он
    // зависит только от протокола, 07_TechStack.md, п.6), поэтому новая
    // черта появится в клиенте сама, без правок здесь.
    std::vector<std::vector<std::pair<std::string, float>>> plantSpecies;
    // Виды травоядных — тем же способом и по той же причине.
    std::vector<std::vector<std::pair<std::string, float>>> herbivoreSpecies;

    // Травоядные — списком, а не слоем по тайлам, как всё остальное выше:
    // они подвижны, их десятки на десятки тысяч клеток, и на одной клетке
    // их может стоять несколько (плотный массив этого просто не выразил бы).
    // Пол и желание приходят строками — клиент их только показывает и не
    // должен знать, какие ещё бывают.
    struct Herbivore {
        int x = 0;
        int y = 0;
        int species = 0;
        float growth = 0.0f;
        std::string sex;
        std::string desire;
    };
    std::vector<Herbivore> herbivores;

    // Параметры, которыми сгенерирован текущий мир — стартовая точка для
    // панели настроек генерации.
    goblins::RegenerationRequest generation{};
    bool hasGeneration = false;
    bool connected = false;

    // Константы законов мира, как их прислал сервер (core/Diagnostics.hpp):
    // группа, имя как в исходнике ядра, значение. Только для показа
    // (оверлей по клавише C). Как и с чертами генома выше — тройки, а не
    // структура с полями: клиент не знает состава этого списка и не должен,
    // новая константа появится в оверлее сама.
    struct Constant {
        std::string group;
        std::string name;
        float value = 0.0f;
    };
    std::vector<Constant> constants;

    // Имя мира, который сейчас в памяти сервера; пустая строка — мир ещё
    // не сохранён (например, только что перегенерирован на экране World
    // Generation).
    std::string currentWorld;

    // Сохранённые миры на сервере (сообщение world_list). worldsReceived
    // отличает "миров нет" от "список ещё не пришёл" — экран выбора мира
    // на этом различии и построен: автоматически создавать новый мир
    // можно только по первому, а не по второму.
    std::vector<goblins::WorldSaveInfo> worlds;
    bool worldsReceived = false;

    // Последнее сообщение сервера о результате операции (notice):
    // сохранение/загрузка мира происходят на сервере, и об ошибке клиент
    // иначе не узнал бы вовсе. noticeAt — момент получения, экраны сами
    // решают, сколько его показывать.
    std::string notice;
    bool noticeIsError = false;
    std::chrono::steady_clock::time_point noticeAt{};
};

// Показывать ли последнее сообщение сервера (notice): оно живёт
// несколько секунд и гаснет. Общая функция, а не логика внутри каждого
// экрана — иначе на разных экранах сообщение жило бы по-разному.
inline bool hasFreshNotice(const WorldState& state,
                           std::chrono::seconds lifetime = std::chrono::seconds(8)) {
    return !state.notice.empty() && (std::chrono::steady_clock::now() - state.noticeAt) < lifetime;
}

// Единственное WebSocket-соединение на весь клиент — устанавливается
// один раз в main() и используется всеми экранами, которым нужны данные
// о мире (07_TechStack.md, п.6: клиент получает данные только по
// протоколу сервера).
class NetworkClient {
public:
    void connect(const std::string& host, int port);
    void disconnect();

    // Текущее состояние мира — разделяемым указателем на неизменяемый
    // снимок, а не копией. Копия состояния — это десяток векторов по
    // числу тайлов Области; экраны запрашивают его каждый кадр (60 раз в
    // секунду), а меняется оно только с приходом сообщения сервера (в
    // разы реже). Поэтому копия делается один раз, при разборе
    // сообщения, а экраны получают указатель на готовый снимок; тот, что
    // они держат, у них из-под рук не изменится. Никогда не nullptr.
    std::shared_ptr<const WorldState> snapshot() const;

    void sendTogglePause();
    void sendRegenerate(const goblins::RegenerationRequest& request);

    // Пустое worldName — запустить новый мир (сервер сгенерирует его
    // текущими параметрами и сразу сохранит); непустое — загрузить
    // сохранённый мир с этим именем.
    void sendStartSimulation(const std::string& worldName = std::string{});
    void sendStopSimulation();

    // Сохранить параметры генерации в config.json сервера. Параметры
    // передаются явно — это ровно то, что сейчас набрано на панели, а не
    // то, чем сгенерирован мир: кнопка "Save values" стоит рядом с
    // ползунками и означает "запомнить вот эти значения", независимо от
    // того, нажимал ли пользователь перед этим "Regenerate".
    void sendSaveGenerationConfig(const goblins::RegenerationRequest& request);

    // Запросить список сохранённых миров (ответ — world_list).
    void sendListWorlds();

    // Сохранить текущее состояние мира. Пустое name — под именем
    // текущего мира (или под новым, если мир ещё не сохранялся).
    void sendSaveWorld(const std::string& name = std::string{});

    // Удалить сохранённый мир по имени.
    void sendDeleteWorld(const std::string& name);

private:
    void handleMessage(const std::string& payload);
    // Разбор списка животных: он одинаков в world_init и в world_delta.
    void applyHerbivores(const nlohmann::json& message);
    // Опубликовать working_ как новый неизменяемый снимок. Единственное
    // место, где состояние копируется.
    void publishState();

    ix::WebSocket webSocket_;

    // Рабочая копия — её трогает только сетевой поток IXWebSocket
    // (единственный, откуда приходят сообщения), поэтому она без
    // мьютекса. Дельта по-другому и не применилась бы: она описывает
    // изменения относительно уже накопленного состояния, а не заменяет
    // его целиком.
    WorldState working_;
    // Множитель для целочисленных слоёв протокола ("scale" из
    // world_init): сервер шлёт доли тысячными долями целых.
    float milliScale_ = 0.001f;

    mutable std::mutex mutex_;
    std::shared_ptr<const WorldState> published_ = std::make_shared<const WorldState>();
};

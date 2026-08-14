#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocketServer.h>

#include "config/Config.hpp"
#include "core/World.hpp"
#include "world/WorldSaveInfo.hpp"

namespace goblins {

// Запрос на запуск симуляции. Пустое worldName — начать новый мир:
// сгенерировать текущими параметрами генерации и сразу сохранить его как
// мир с нулевым тиком. Непустое — загрузить сохранённый мир с этим
// именем (см. server/WorldSave.hpp).
struct StartSimulationRequest {
    std::string worldName;
};

// Запрос на сохранение текущего состояния мира. Пустое name — сохранить
// под именем мира, который сейчас в памяти (а если он ещё ни разу не
// сохранялся — под новым сгенерированным именем).
struct SaveWorldRequest {
    std::string name;
};

// Сетевой слой сервера: транспорт (WebSocket) и сериализация (JSON) живут
// здесь, а не в core — согласно "Все взаимодействия происходят через
// интерфейсы" (02_CorePrinciples.md) и границам модулей (07_TechStack.md,
// п.6: core не знает о server, server не меняет core).
//
// Протокол (версия 12):
//   Состояние мира уходит клиенту двумя разными сообщениями, потому что
//   оно состоит из двух разных по природе частей. Полный world_init —
//   всё, включая то, что между регенерациями не меняется вообще
//   (каменистость, булыжники, источники, виды травы, параметры
//   генерации). Дельта world_delta — только те клетки, что изменились с
//   прошлой рассылки. Полный снапшот на каждый тик означал бы десятки
//   тысяч чисел пять раз в секунду ради нескольких сотен изменившихся
//   клеток; при этом очередь отправки сокета неограниченна, и клиент,
//   не успевающий их принимать, отставал бы всё сильнее — счётчик тика
//   продолжал бы расти уже после остановки мира.
//
//   Сервер -> клиент, при подключении, после регенерации и после
//   загрузки/создания мира (всё, что нужно, чтобы построить мир с нуля):
//     {"type": "world_init", "area": {"width", "height"}, "tick": N,
//      "paused": bool, "world": "имя текущего мира или пустая строка",
//      "scale": 1000,  -- делитель для целочисленных слоёв (см. ниже)
//      "terrain_seed": N, "terrain": {...},
//      "boulder_count": N, "boulder_seed": N,
//      "plants": {...}, "plant_seed": N,  -- вместе с terrain/*_seed это
//              полный RegenerationRequest: панель настроек клиента строится
//              из этого сообщения целиком, без вкомпилированных умолчаний
//      "constants": [{"group", "name", "value"}, ...],  -- числа, зашитые
//              в законы мира (core/Diagnostics.hpp): не настраиваются и
//              никогда не меняются, клиент показывает их только для
//              чтения (оверлей по клавише C)
//      "boulders": [{"x", "y"}, ...],
//      "water_sources": [{"x", "y"}, ...],  -- истоки рек + "родники"
//              (WaterSourceComponent), тег без данных, как boulders
//      "plant_species": [{"species": N, <черты генома>}, ...],
//      "layers": {"rockiness", "moisture", "compaction", "minerals",
//                 "height", "water", "humus", "species", "growth"}}
//              -- плоские массивы целых, row-major, по одному значению на
//              тайл. Целые, а не float: в JSON число с плавающей точкой
//              печатается как double (два десятка знаков на значение), а
//              плотный массив тут — на всю Область. moisture, compaction,
//              rockiness, height и water — в тысячных долях (делить на
//              "scale"), growth — целые проценты, minerals и humus —
//              счётные величины как есть, species — индекс вида или -1
//              (клетка пуста). water == 0 значит "воды нет" (на сервере
//              у тайла просто нет WaterComponent), humus == 0 — "перегноя
//              нет".
//   Сервер -> клиент, не чаще snapshot_interval_ms, только если что-то
//   изменилось (на паузе не шлётся вовсе — миру нечего сообщать):
//     {"type": "world_delta", "tick": N, "paused": bool,
//      "moisture": [индекс, значение, индекс, значение, ...], ...}
//              -- те же слои, что в "layers" у world_init (кроме
//              статических: каменистость, булыжники, источники и виды
//              травы меняются только регенерацией, а она шлёт новый
//              world_init). Пары "индекс тайла - новое значение", только
//              изменившиеся клетки; слой без изменений в сообщение не
//              попадает вовсе.
//   Сервер -> клиент, сразу при подключении и после каждого сохранения:
//     {"type": "world_list", "current": "имя текущего мира",
//      "worlds": [WorldSaveInfo, ...]}  -- см. shared/world/WorldSaveInfo.hpp
//   Сервер -> клиент, при изменении паузы (рассылается всем клиентам):
//     {"type": "pause_state", "paused": bool}
//   Сервер -> клиент, результат операции, у которой нет своего ответа
//   (сохранение/загрузка мира) — клиенту нужно показать, что именно
//   произошло, особенно когда произошла ошибка:
//     {"type": "notice", "level": "info"|"error", "text": "..."}
//   Клиент -> сервер, запрос переключить паузу:
//     {"type": "toggle_pause"}
//   Клиент -> сервер, запрос остановить симуляцию (кнопка "Back" на
//   экране симуляции) — не toggle, а гарантированная пауза:
//     {"type": "stop_simulation"}
//   Клиент -> сервер, запрос сохранить параметры генерации в config.json
//   сервера, чтобы они стали значениями по умолчанию при следующем запуске:
//     {"type": "save_generation_config", "params": RegenerationRequest}
//     Сохраняется именно присланное — то, что набрано на панели клиента,
//     даже если мир этими значениями ещё не перегенерирован. Без "params"
//     (старый клиент) сохраняется currentGenerationConfig_.
//   Клиент -> сервер, запрос перегенерировать мир (почву/воду/булыжники):
//     {"type": "regenerate", "params": RegenerationRequest}
//     Сервер выполняет это на потоке GameLoop (не сразу в сетевом
//     колбэке — ECS registry не потокобезопасен), затем рассылает
//     свежий world_init всем подключённым клиентам.
//   Клиент -> сервер, запрос прислать список сохранённых миров:
//     {"type": "list_worlds"}
//   Клиент -> сервер, запрос сохранить текущее состояние мира:
//     {"type": "save_world", "name": "необязательное имя"}
//     Выполняется на потоке GameLoop (чтение ECS registry), после чего
//     сервер рассылает свежий world_list.
//   Клиент -> сервер, запрос удалить сохранённый мир:
//     {"type": "delete_world", "name": "имя"}
//     Только файловый ввод-вывод (WorldSave::deleteWorld), ECS registry не
//     трогает — выполняется сразу на сетевом потоке, как list_worlds.
//     После удаления рассылается свежий world_list и notice с результатом.
//   Клиент -> сервер, запрос запустить симуляцию:
//     {"type": "start_simulation", "world": "необязательное имя мира"}
//     До этой команды сервер только слушает WebSocket — мир не
//     сгенерирован и GameLoop не тикает (paused == true). С именем мира
//     сервер загружает сохранение, без имени — генерирует новый мир
//     текущим currentGenerationConfig_ и сразу сохраняет его (мир с
//     нулевым тиком). И то, и другое — на потоке GameLoop, как
//     regenerate; затем пауза снимается.
class NetworkServer {
public:
    // paused — ссылка на флаг паузы игрового цикла (GameLoop::paused).
    // NetworkServer управляет им напрямую по запросу клиента: так у
    // состояния паузы остаётся один источник истины, а не две
    // независимые копии (в GameLoop и в NetworkServer), которые могли бы
    // разойтись.
    // baseConfig/configPath — нужны только для save_generation_config:
    // сохраняем на диск полный ServerConfig (host/port/area/tick_* как
    // были при запуске), подменив в нём только поля генерации текущим
    // currentGenerationConfig_.
    // savesDirectory — каталог сохранённых миров: сам сетевой слой
    // только читает из него список миров (list_worlds), запись мира
    // делает main.cpp с потока GameLoop.
    NetworkServer(const World& world, const std::string& host, int port, std::atomic<bool>& paused,
                  ServerConfig baseConfig, std::string configPath, std::filesystem::path savesDirectory);

    // Возвращает false, если порт не удалось занять — например, он уже
    // используется другим процессом.
    bool start();
    void stop();

    // Разослать накопившиеся изменения мира. Вызывать ТОЛЬКО с потока
    // GameLoop: читает ECS registry, который не потокобезопасен между
    // потоками (по той же причине, что и takePendingRegeneration).
    // Вызывается и после каждого тика, и на каждой итерации цикла:
    // второе нужно, чтобы подключившийся во время паузы клиент получил
    // мир, не дожидаясь следующего тика (которого на паузе не будет).
    //
    // Сама решает, что именно и нужно ли вообще отправлять:
    //   - если клиентов нет — не строит сообщение вообще (мир существует
    //     независимо от наблюдателя, но сериализовать его некому);
    //   - если запрошен полный ресинк (requestFullResync) — шлёт
    //     world_init и запоминает его как точку отсчёта для дельт;
    //   - иначе, не чаще snapshot_interval_ms, — дельту от последней
    //     реально отправленной точки; пустую (ничего не изменилось) не
    //     шлёт;
    //   - если у кого-то из клиентов очередь отправки больше
    //     kSnapshotBacklogBytes — пропускает рассылку целиком: клиент не
    //     успевает принимать, и копить для него сообщения значит
    //     показывать ему прошлое. Точка отсчёта при этом не сдвигается,
    //     поэтому пропущенные изменения войдут в следующую дельту.
    // force = true — игнорировать интервал (после регенерации, загрузки
    // и смены паузы состояние должно уйти немедленно).
    void publish(bool force = false);

    // Пометить, что следующая рассылка должна быть полной (world_init).
    // Нужно после всего, что делает прошлое состояние клиента негодным
    // как точка отсчёта для дельт: регенерация, загрузка мира, новый
    // подключившийся клиент. Флаг снимается только когда world_init
    // действительно отправлен, поэтому дельта никогда не ляжет на чужую
    // основу.
    void requestFullResync();

    // Параметры, которыми сейчас сгенерирован мир — включаются в
    // снапшот (это отправная точка для панели настроек клиента). Вызвать
    // один раз после начальной генерации и затем каждый раз после
    // успешной регенерации.
    void setCurrentGenerationConfig(const RegenerationRequest& config);

    // Текущий конфиг генерации (копия) — используется при обработке
    // start_simulation, чтобы сгенерировать мир тем же конфигом, что уже
    // виден клиенту в панели настроек.
    RegenerationRequest currentGenerationConfig() const;

    // Имя мира, который сейчас в памяти (пустая строка — мир ещё ни разу
    // не сохранялся: например, только что перегенерирован на экране
    // World Generation). Попадает в снапшот и в world_list, чтобы клиент
    // показывал, какой именно мир он видит.
    void setCurrentWorldName(const std::string& name);
    std::string currentWorldName() const;

    // Если клиент прислал запрос на регенерацию — возвращает его и
    // очищает очередь (иначе nullopt). Вызывать только с потока, где
    // крутится GameLoop, не из сетевого колбэка: сама регенерация трогает
    // ECS registry, который не потокобезопасен между потоками.
    std::optional<RegenerationRequest> takePendingRegeneration();

    // Аналогично takePendingRegeneration — запрос на запуск симуляции
    // (новый мир или загрузка сохранённого) и запрос на сохранение
    // текущего состояния мира. Оба трогают ECS registry (генерация,
    // загрузка, чтение всех Entity при записи файла), поэтому
    // выполняются на потоке GameLoop.
    std::optional<StartSimulationRequest> takePendingStartSimulation();
    std::optional<SaveWorldRequest> takePendingSaveWorld();

    // Рассылает всем клиентам текущее состояние паузы. Обычно вызывается
    // изнутри handleClientMessage (toggle_pause), но также нужна снаружи —
    // после того, как main.cpp снимает паузу по завершении обработки
    // start_simulation на потоке GameLoop.
    void broadcastPauseState();

    // Сколько явных команд паузы (toggle_pause / stop_simulation) пришло
    // от клиентов за всё время. Пауза — единственное состояние, которое
    // сетевой поток меняет сам и сразу, тогда как генерация и загрузка
    // мира идут на потоке GameLoop и занимают заметное время. Если за это
    // время клиент успел прислать stop_simulation (нажал "Back"), снимать
    // паузу по завершении загрузки уже нельзя — иначе мир продолжил бы
    // тикать после выхода с экрана симуляции. Поток GameLoop запоминает
    // счётчик до работы и сравнивает после.
    std::uint64_t pauseCommandCount() const;

    // Рассылает всем клиентам список сохранённых миров — вызывать после
    // каждой записи мира на диск, чтобы список в меню не устаревал.
    void broadcastWorldList();

    // Сообщение о результате операции (сохранение/загрузка мира) — то,
    // что клиенту нужно показать пользователю. Ошибки без этого были бы
    // видны только в консоли сервера.
    void broadcastNotice(const std::string& level, const std::string& text);

private:
    // Состояние мира в том виде, в котором оно уходит по сети: плоские
    // массивы целых, row-major, по одному значению на тайл. Именно от
    // такого снимка считаются дельты — сравнивать надо ровно то, что
    // клиент уже получил, а не исходные float (после округления до
    // тысячных изменения меньше тысячной для клиента не существует, и
    // гонять их по сети незачем).
    struct LayerSnapshot {
        int width = 0;
        int height = 0;
        std::vector<int> moisture;
        std::vector<int> compaction;
        std::vector<int> minerals;
        std::vector<int> terrainHeight;
        std::vector<int> water;
        std::vector<int> humus;
        std::vector<int> species;
        std::vector<int> growth;
        // Каменистость меняется только генерацией, поэтому в дельты не
        // входит — но лежит здесь же, чтобы world_init собирался из
        // одного и того же снимка, что и дельты.
        std::vector<int> rockiness;

        void resize(int w, int h);
    };

    // Читает ECS registry в out. Только с потока GameLoop.
    void captureLayers(LayerSnapshot& out) const;
    std::string buildInitMessage(const LayerSnapshot& layers) const;
    // Возвращает пустую строку, если ничего не изменилось.
    std::string buildDeltaMessage(const LayerSnapshot& previous, const LayerSnapshot& current) const;
    std::string buildWorldListMessage() const;
    void handleClientMessage(const std::string& payload);
    void broadcastToAll(const std::string& payload);
    // true, если хоть один клиент не разгрёб свою очередь отправки.
    bool clientsAreBehind();

    const World& world_;
    ix::WebSocketServer server_;
    std::atomic<bool>& paused_;
    std::atomic<std::uint64_t> pauseCommandCount_{0};

    // Под этим мьютексом — описание того, какой мир сейчас в памяти:
    // чем он сгенерирован и под каким именем сохранён. Читается из
    // сетевого потока (сборка снапшота), пишется с потока GameLoop.
    mutable std::mutex generationConfigMutex_;
    RegenerationRequest currentGenerationConfig_;
    std::string currentWorldName_;

    ServerConfig baseConfig_;
    std::string configPath_;
    std::filesystem::path savesDirectory_;

    std::mutex pendingRegenerationMutex_;
    std::optional<RegenerationRequest> pendingRegeneration_;

    std::mutex pendingStartMutex_;
    std::optional<StartSimulationRequest> pendingStart_;

    std::mutex pendingSaveWorldMutex_;
    std::optional<SaveWorldRequest> pendingSaveWorld_;

    // Взводится с сетевого потока (подключение клиента) и с потока
    // GameLoop (регенерация, загрузка) — поэтому атомарный. Всё
    // остальное ниже трогает только поток GameLoop, внутри publish().
    std::atomic<bool> needsFullResync_{true};
    // Последнее реально отправленное состояние — точка отсчёта для
    // следующей дельты, и current_ как переиспользуемый буфер (чтобы не
    // выделять девять векторов на каждую рассылку).
    LayerSnapshot sent_;
    LayerSnapshot current_;
    bool sentValid_ = false;
    std::chrono::steady_clock::time_point lastPublish_{};
    // Тик и пауза, отправленные последними: дельта нужна и тогда, когда
    // не изменилась ни одна клетка (номер тика в интерфейсе клиента всё
    // равно должен идти), и не нужна, когда не изменилось вообще ничего.
    std::uint64_t sentTick_ = 0;
    bool sentPaused_ = true;
};

} // namespace goblins

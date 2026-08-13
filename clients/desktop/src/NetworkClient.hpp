#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "config/Config.hpp"
#include "world/WorldSaveInfo.hpp"

// Снимок состояния мира на стороне клиента — обновляется из сетевого
// потока (IXWebSocket), читается из потока рендера. Общий для экранов
// "Генерация мира" и "Симуляция": обе показывают один и тот же мир,
// просто по-разному его отображают.
struct WorldState {
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

    // Параметры, которыми сгенерирован текущий мир — стартовая точка для
    // панели настроек генерации.
    goblins::RegenerationRequest generation{};
    bool hasGeneration = false;
    bool connected = false;

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

    WorldState snapshot() const;

    void sendTogglePause();
    void sendRegenerate(const goblins::RegenerationRequest& request);

    // Пустое worldName — запустить новый мир (сервер сгенерирует его
    // текущими параметрами и сразу сохранит); непустое — загрузить
    // сохранённый мир с этим именем.
    void sendStartSimulation(const std::string& worldName = std::string{});
    void sendStopSimulation();
    void sendSaveGenerationConfig();

    // Запросить список сохранённых миров (ответ — world_list).
    void sendListWorlds();

    // Сохранить текущее состояние мира. Пустое name — под именем
    // текущего мира (или под новым, если мир ещё не сохранялся).
    void sendSaveWorld(const std::string& name = std::string{});

    // Удалить сохранённый мир по имени.
    void sendDeleteWorld(const std::string& name);

private:
    void handleMessage(const std::string& payload);

    ix::WebSocket webSocket_;
    mutable std::mutex mutex_;
    WorldState state_;
};

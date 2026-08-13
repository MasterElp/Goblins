#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

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
// Протокол (версия 8):
//   Сервер -> клиент, сразу при подключении, после регенерации и после
//   КАЖДОГО тика (HydrologySystem меняет почву/воду непрерывно — клиент
//   должен видеть свежее состояние каждый тик, а не только номер):
//     {"type": "world_snapshot", "area": {"width", "height"}, "tick": N,
//      "paused": bool, "world": "имя текущего мира или пустая строка",
//      "terrain_seed": N, "terrain": {...},
//      "boulder_count": N, "boulder_seed": N,
//      "boulders": [{"x", "y"}, ...],
//      "soil": {"moisture": [...], "rockiness": [...], "compaction": [...],
//               "minerals": [...]},
//              -- плоские массивы, row-major, по одному значению на тайл
//              (float, кроме minerals — целое количество)
//      "water": [{"x", "y", "depth"}, ...]}  -- только тайлы с водой
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
//   Клиент -> сервер, запрос сохранить текущие параметры генерации
//   (currentGenerationConfig_) в config.json сервера, чтобы они стали
//   значениями по умолчанию при следующем запуске:
//     {"type": "save_generation_config"}
//   Клиент -> сервер, запрос перегенерировать мир (почву/воду/булыжники):
//     {"type": "regenerate", "params": RegenerationRequest}
//     Сервер выполняет это на потоке GameLoop (не сразу в сетевом
//     колбэке — ECS registry не потокобезопасен), затем рассылает
//     свежий world_snapshot всем подключённым клиентам.
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

    // Полный снапшот мира всем подключённым клиентам. Вызывается из
    // GameLoop::onTickComplete после каждого тика (HydrologySystem
    // непрерывно меняет почву/воду — клиенту нужно видеть свежее
    // состояние, а не только номер тика), а также вручную после
    // регенерации, чтобы все подключённые клиенты (не только тот, кто
    // запросил) увидели новую карту.
    void broadcastSnapshot();

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
    std::string buildSnapshotMessage() const;
    std::string buildWorldListMessage() const;
    void handleClientMessage(const std::string& payload);
    void broadcastToAll(const std::string& payload);

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
};

} // namespace goblins

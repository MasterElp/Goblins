#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <ixwebsocket/IXWebSocketServer.h>

#include "config/Config.hpp"
#include "core/World.hpp"

namespace goblins {

// Сетевой слой сервера: транспорт (WebSocket) и сериализация (JSON) живут
// здесь, а не в core — согласно "Все взаимодействия происходят через
// интерфейсы" (02_CorePrinciples.md) и границам модулей (07_TechStack.md,
// п.6: core не знает о server, server не меняет core).
//
// Протокол (версия 4):
//   Сервер -> клиент, сразу при подключении и после регенерации:
//     {"type": "world_snapshot", "area": {"width", "height"}, "tick": N,
//      "paused": bool, "terrain_seed": N, "terrain": {...},
//      "boulder_count": N, "boulder_seed": N,
//      "boulders": [{"x", "y"}, ...],
//      "soil": {"moisture": [...], "rockiness": [...], "compaction": [...]},
//              -- плоские массивы, row-major, по одному float на тайл
//      "water": [{"x", "y", "depth"}, ...]}  -- только тайлы с водой
//   Сервер -> клиент, после каждого тика:
//     {"type": "tick", "tick": N}
//   Сервер -> клиент, при изменении паузы (рассылается всем клиентам):
//     {"type": "pause_state", "paused": bool}
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
//   Клиент -> сервер, запрос запустить симуляцию (без payload):
//     {"type": "start_simulation"}
//     До этой команды сервер только слушает WebSocket — мир не
//     сгенерирован и GameLoop не тикает (paused == true). Сервер
//     генерирует мир текущим currentGenerationConfig_ (тем же потоком,
//     что и regenerate) и снимает паузу.
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
    NetworkServer(const World& world, const std::string& host, int port, std::atomic<bool>& paused,
                  ServerConfig baseConfig, std::string configPath);

    // Возвращает false, если порт не удалось занять — например, он уже
    // используется другим процессом.
    bool start();
    void stop();

    // Вызывается из GameLoop::onTickComplete — рассылает номер тика всем
    // подключённым клиентам.
    void broadcastTick(std::uint64_t tick);

    // Полный снапшот заново — вызвать после регенерации мира, чтобы все
    // подключённые клиенты (не только тот, кто запросил) увидели новую
    // карту.
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

    // Если клиент прислал запрос на регенерацию — возвращает его и
    // очищает очередь (иначе nullopt). Вызывать только с потока, где
    // крутится GameLoop, не из сетевого колбэка: сама регенерация трогает
    // ECS registry, который не потокобезопасен между потоками.
    std::optional<RegenerationRequest> takePendingRegeneration();

    // Аналогично takePendingRegeneration, но для запроса на запуск
    // симуляции (start_simulation) — просто флаг без параметров.
    bool takePendingStartSimulation();

    // Рассылает всем клиентам текущее состояние паузы. Обычно вызывается
    // изнутри handleClientMessage (toggle_pause), но также нужна снаружи —
    // после того, как main.cpp снимает паузу по завершении обработки
    // start_simulation на потоке GameLoop.
    void broadcastPauseState();

private:
    std::string buildSnapshotMessage() const;
    void handleClientMessage(const std::string& payload);
    void broadcastToAll(const std::string& payload);

    const World& world_;
    ix::WebSocketServer server_;
    std::atomic<bool>& paused_;

    mutable std::mutex generationConfigMutex_;
    RegenerationRequest currentGenerationConfig_;

    ServerConfig baseConfig_;
    std::string configPath_;

    std::mutex pendingRegenerationMutex_;
    std::optional<RegenerationRequest> pendingRegeneration_;

    std::mutex pendingStartMutex_;
    bool pendingStart_ = false;
};

} // namespace goblins

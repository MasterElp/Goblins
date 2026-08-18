"""Проверка дельты животных: сходится ли список, собранный из изменений,
с тем, который сервер присылает целиком.

Животные в дельте описаны изменениями (см. "animals" в протоколе,
server/NetworkServer.hpp), а изменения ссылаются на ПРЕЖНИЙ список
индексами — то есть проверка тут одна на всё: собрать список ровно так,
как это делает клиент, и время от времени сличить с полным world_init.

Расхождение означает, что на карте животные окажутся не там, где они есть
в мире. Глазами это почти не поймать: список правится каждый тик, ошибка
копится молча, а зверь, уехавший на клетку в сторону, выглядит просто
зверем.

Сличать можно только на одном и том же тике, иначе разойдётся не дельта, а
время: полный список приезжает позже последней дельты, и мир за это время
успевает шагнуть. Поэтому мир на время сличения останавливается, а полный
список выпрашивается вторым подключением — сервер на нового наблюдателя
рассылает world_init всем.

Заодно, отдельной проверкой, — что клиент может отвернуться ("updates"):
пока он отвернулся, ему не приходит ничего, а вернувшемуся приходит мир
целиком.

Запуск (сервер поднимается сам):
    python3 tools/check_animal_delta.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_probe import WebSocketProbe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "build", "server", "server")
PORT = 9107
# Сличений с полным списком. Каждое стоит серверу world_init на всю
# Область, поэтому их немного, но между ними проходят сотни тиков — как раз
# столько, чтобы успели и родиться, и умереть.
RESYNCS = 6
TICKS_BETWEEN = 150


def apply_changes(animals, changes):
    """То же самое, что делает NetworkClient::applyAnimalChanges."""
    triples = changes.get("pos", [])
    for p in range(0, len(triples) - 2, 3):
        animals[triples[p]]["x"] = triples[p + 1]
        animals[triples[p]]["y"] = triples[p + 2]
    for key in ("growth", "health", "desire"):
        pairs = changes.get(key, [])
        for p in range(0, len(pairs) - 1, 2):
            animals[pairs[p]][key] = pairs[p + 1]
    gone = set(changes.get("gone", []))
    if gone:
        animals = [a for i, a in enumerate(animals) if i not in gone]
    born = changes.get("born", [])
    if born:
        animals = sorted(animals + list(born), key=lambda a: a["id"])
    return animals


def report(applied, animals, full):
    print(f"РАСХОЖДЕНИЕ на тике {applied}: собрано {len(animals)}, у сервера {len(full)}")
    ids_have = {a["id"] for a in animals}
    ids_want = {a["id"] for a in full}
    if ids_have != ids_want:
        print(f"  лишних {len(ids_have - ids_want)}, недостаёт {len(ids_want - ids_have)}")
        return
    wrong = [(h, w) for h, w in zip(animals, full) if h != w]
    print(f"  состав тот же, но {len(wrong)} карточек не совпадают")
    for have, want in wrong[:3]:
        print(f"   собрано:   {have}")
        print(f"   у сервера: {want}")


class Session:
    """Разговор с сервером его же протоколом, с накоплением списка."""

    def __init__(self, port):
        self.probe = WebSocketProbe(port=port)
        self.animals = None
        self.tick = 0
        self.deltas = 0

    def take(self, message):
        kind = message.get("type")
        if kind == "world_init":
            self.animals = sorted(message.get("animals") or [], key=lambda a: a["id"])
            self.tick = message.get("tick", self.tick)
        elif kind == "world_delta":
            self.tick = message.get("tick", self.tick)
            if self.animals is not None and "animals" in message:
                self.deltas += 1
                self.animals = apply_changes(self.animals, message["animals"])
        return kind

    def run_to(self, tick):
        deadline = time.time() + 600
        while self.tick < tick and time.time() < deadline:
            self.take(self.probe.recv())

    def wait_for(self, kind, seconds=60):
        deadline = time.time() + seconds
        while time.time() < deadline:
            message = self.probe.recv()
            if message.get("type") == kind:
                return message
            self.take(message)
        return None


def check_suspension():
    """Отвернувшемуся клиенту сервер не шлёт ничего, вернувшемуся — мир
    целиком.

    Своим подключением, а не общим: проверка нарочно слушает тишину, и
    мешать это с накоплением списка незачем.
    """
    probe = WebSocketProbe(port=PORT)
    probe.send({"type": "start_simulation"})
    if all(probe.recv(60).get("type") != "world_delta" for _ in range(200)):
        print("Мир не пошёл — проверять отворачивание не на чем")
        return 1

    probe.send({"type": "updates", "enabled": False})
    # Первые кадры могут быть уже в пути: даём им дойти, а потом слушаем
    # тишину. Тишина дольше десятка snapshot_interval_ms и означает, что
    # сервер замолчал.
    for _ in range(20):
        if probe.recv(1.0) is None:
            break
    else:
        print("Сервер продолжает слать отвернувшемуся клиенту")
        return 1

    probe.send({"type": "updates", "enabled": True})
    for _ in range(20):
        message = probe.recv(30)
        if message is None:
            break
        if message.get("type") == "world_init":
            print("Отворачивание работает: тишина, пока отвернулся, "
                  "и мир целиком по возвращении")
            return 0
    print("Вернувшемуся клиенту не пришёл world_init")
    return 1


def main():
    if not os.path.exists(SERVER):
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-animal-delta-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    config["tick_interval_ms"] = 1
    # Не единица: разбор JSON на питоне медленнее, чем сервер успевает
    # слать, и проверка ушла бы в вечную догонялку. Дельта от этого не
    # меняется — она описывает изменения, а не тики.
    config["snapshot_interval_ms"] = 50
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([SERVER, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        session = None
        for _ in range(60):
            try:
                session = Session(PORT)
                break
            except OSError:
                time.sleep(1)
        if session is None:
            print("Сервер не поднялся")
            return 1

        session.probe.send({"type": "start_simulation"})
        for check in range(1, RESYNCS + 1):
            session.run_to(check * TICKS_BETWEEN)

            # Мир на паузу: полный список приедет позже последней дельты, а
            # сравнивать надо одно и то же время.
            session.probe.send({"type": "toggle_pause"})
            if session.wait_for("pause_state") is None:
                print("Сервер не подтвердил паузу")
                return 1

            # Полный список выпрашиваем вторым подключением: на нового
            # наблюдателя сервер рассылает world_init всем. Ничего при этом
            # не теряется по дороге — в отличие от "отвернуться и
            # посмотреть снова", которое как раз для того и сделано, чтобы
            # не слать отвернувшемуся ничего.
            watcher = WebSocketProbe(port=PORT)
            full_message = session.wait_for("world_init")
            watcher.s.close()
            if full_message is None:
                print("Сервер не прислал world_init новому наблюдателю")
                return 1
            collected = list(session.animals)
            deltas = session.deltas
            full = sorted(full_message.get("animals") or [], key=lambda a: a["id"])
            if full_message.get("tick") != session.tick:
                print(f"Мир шагнул на паузе: дельты по тик {session.tick}, "
                      f"полный список на тик {full_message.get('tick')}")
                return 1
            if collected != full:
                report(session.tick, collected, full)
                return 1
            print(f"  сличение {check}/{RESYNCS} на тике {session.tick}: "
                  f"{len(full)} животных, {deltas} дельт позади — сходится")

            session.animals = full
            session.deltas = 0
            session.probe.send({"type": "toggle_pause"})

        print(f"Дельта животных сходится: {RESYNCS} сличений, тик {session.tick}")
        # Закрыть, а не бросить: неразгребаемый клиент останавливает
        # рассылку ВСЕМ (см. clientsAreBehind), и следующая проверка
        # ждала бы мира, которого сервер никому не шлёт.
        session.probe.s.close()
        return check_suspension()
    finally:
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

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
успевает шагнуть. Поэтому мир на время сличения останавливается.

Разговор идёт по ДВУМ подключениям, и это не удобство, а обход свойства
сервера. Одно слушает мир и ничего не просит, второе просит и ничего не
слушает: сервер обслуживает соединение одним потоком, и пока тот занят
отправкой дельт, входящие кадры того же соединения он не читает. Просьба о
паузе, поданная по слушающему подключению, не была прочитана НИ РАЗУ из
двадцати — мир уходил на сотни тиков вперёд, а в журнале сервера не
появлялось ни строчки. По молчащему подключению та же просьба доходит
всегда. Дело не в скорости наблюдателя: он успевал за рассылкой.

Заодно, отдельной проверкой, — что клиент может отвернуться ("updates"):
пока он отвернулся, мир ему не приходит, а вернувшемуся приходит целиком.
Проверяется это на том же молчащем подключении, по той же причине: попросить
"не показывай мне мир" может только тот, кого сейчас не заливают.

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

from server_binary import find_server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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
    # "fatigue" и "carried" есть только у гоблинов, и у животных этих ключей
    # в сообщении просто не бывает — общий список тут ничего не путает. На
    # той стороне они тоже правятся вместе с остальными парами
    # (applyCreatureChanges в NetworkClient.cpp), между правками и
    # удалениями: индексы во всех них считаны в ПРЕЖНЕМ списке.
    for key in ("growth", "health", "desire", "fatigue", "carried"):
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


def report(what, applied, animals, full):
    print(f"РАСХОЖДЕНИЕ ({what}) на тике {applied}: собрано {len(animals)}, "
          f"у сервера {len(full)}")
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
        # Два подключения, и разделение между ними несущее.
        #
        # probe СЛУШАЕТ мир и ничего не просит. control ПРОСИТ и ничего не
        # слушает: первым делом он отворачивается ("updates": false), и
        # сервер больше не шлёт ему ни байта.
        #
        # Иначе команды не доходят. Сервер обслуживает соединение одним
        # потоком, и пока тот занят отправкой дельт, входящие кадры того же
        # соединения он не читает. Измерено: на непрерывно принимающем
        # подключении из двадцати просьб о паузе не была прочитана ни одна —
        # мир уходил на сотни тиков вперёд, а в журнале сервера не появлялось
        # ни строчки о паузе. Дело не в скорости наблюдателя: он успевал за
        # рассылкой, отставания не было. На отвернувшемся же подключении
        # команда читается сразу, десять раз из десяти.
        self.probe = WebSocketProbe(port=port)
        self.control = WebSocketProbe(port=port)
        self.control.send({"type": "updates", "enabled": False})
        while self.control.recv(1.0) is not None:
            pass
        self.animals = None
        # Гоблины — второй такой же список. Собирается тем же apply_changes:
        # на той стороне провода обе дельты строит один и тот же шаблон
        # (creaturesDeltaJson в NetworkServer.cpp), и разными они быть не
        # могут по устройству.
        self.goblins = None
        self.tick = 0
        self.deltas = 0

    def command(self, message):
        """Просьба к серверу — всегда по молчащему подключению."""
        self.control.send(message)

    def take(self, message):
        kind = message.get("type")
        if kind == "world_init":
            self.animals = sorted(message.get("animals") or [], key=lambda a: a["id"])
            self.goblins = sorted(message.get("goblins") or [], key=lambda a: a["id"])
            self.tick = message.get("tick", self.tick)
        elif kind == "world_delta":
            self.tick = message.get("tick", self.tick)
            if self.animals is not None and "animals" in message:
                self.deltas += 1
                self.animals = apply_changes(self.animals, message["animals"])
            if self.goblins is not None and "goblins" in message:
                self.deltas += 1
                self.goblins = apply_changes(self.goblins, message["goblins"])
        return kind

    def run_to(self, tick):
        deadline = time.time() + 600
        while self.tick < tick:
            left = deadline - time.time()
            if left <= 0:
                return False
            message = self.probe.recv(left)
            if message is None:
                return False
            self.take(message)
        return True

    def wait_for(self, kind, seconds=60):
        # Таймаут передаётся в recv, а не проверяется между сообщениями:
        # без него recv блокируется навсегда, и обещанный здесь срок
        # действовал только пока сервер что-нибудь шлёт. Замолчавший сервер
        # подвешивал проверку насмерть вместо того, чтобы назвать причину.
        deadline = time.time() + seconds
        while True:
            left = deadline - time.time()
            if left <= 0:
                return None
            message = self.probe.recv(left)
            if message is None:
                return None
            if message.get("type") == kind:
                return message
            self.take(message)

    def settle(self, seconds=15.0, quiet=1.0):
        """Дочитать всё, что сервер ещё шлёт, и дождаться тишины.

        Нужно ровно затем, чтобы сличать было что. Мир идёт быстрее, чем
        рассылка (snapshot_interval_ms), поэтому между последней полученной
        дельтой и вставшим миром почти всегда остаётся неразосланный хвост:
        наблюдатель знает мир по тик N, а мир замер на N+1. Дельта за этот
        последний тик приходит — просто позже, следующей рассылкой. Не
        дождавшись её, проверка сличала два разных времени и объявляла
        расхождением то, что им не было.

        Тишина длиной в quiet и означает "мир встал и всё разослано":
        идущий мир шлёт заметно чаще.
        """
        deadline = time.time() + seconds
        while time.time() < deadline:
            message = self.probe.recv(quiet)
            if message is None:
                return True
            self.take(message)
        return False

    def full_snapshot(self, seconds=30):
        """Мир целиком, на текущем тике.

        Просьбу подаёт командное подключение: вернувшийся наблюдатель
        заставляет сервер разослать world_init ВСЕМ (см. requestFullResync
        в NetworkServer). Отвернуться обратно можно сразу же — просьба уже
        принята, а слушать командному нечего; читаем мир там же, где и всё
        остальное, слушающим подключением.

        Отворачивать ради этого само слушающее подключение нельзя: тогда
        отвёрнутыми окажутся оба, сервер решит, что смотреть некому
        (anyoneWatching), и не пришлёт мира вообще никому.

        Звать можно только на стоящем мире: иначе между дельтой и полным
        списком снова окажутся разные тики.
        """
        self.control.send({"type": "updates", "enabled": True})
        self.control.send({"type": "updates", "enabled": False})
        full = self.wait_for("world_init", seconds)
        # Командному копия достаться не должна (оно уже отвернулось), но
        # если успела — дочитать: неразобранное у любого из клиентов
        # останавливает рассылку всем (clientsAreBehind).
        while self.control.recv(0.2) is not None:
            pass
        return full


def check_suspension(session):
    """Отвернувшийся не получает состояния мира, вернувшийся получает его
    целиком.

    Проверяется на командном подключении, потому что оно отвернулось ДО
    того, как сервер начал что-либо слать (см. Session.__init__), — и это
    единственный способ, которым отворачивание вообще достижимо.

    Отвернуться посреди рассылки нельзя, и это свойство сервера, а не
    проверки: пока он льёт подключению дельты, он не читает с него команд.
    Измерено здесь же — на залитом подключении просьба отвернуться не была
    услышана ни разу, шестьсот кадров продолжали идти и через минуту.
    Поэтому "клиент свернул окно и перестал получать мир" работает только
    у того, кто успел попросить в тишине. Проверять то, чего сервер не
    умеет, эта проверка не берётся — она проверяет то, что он умеет.
    """
    control = session.control

    # Мир идёт (последнее сличение сняло паузу), а отвернувшемуся не
    # приходит ничего.
    deadline = time.time() + 5.0
    while time.time() < deadline:
        message = control.recv(deadline - time.time())
        if message is None:
            continue
        if message.get("type") in ("world_delta", "world_init"):
            print(f"Сервер шлёт мир отвернувшемуся клиенту: "
                  f"{message.get('type')} на тике {message.get('tick')}")
            return 1

    # А вернувшемуся — мир целиком.
    control.send({"type": "updates", "enabled": True})
    deadline = time.time() + 60
    while time.time() < deadline:
        # Слушающее подключение обязано разгребаться и сейчас: сервер не
        # рассылает НИКОМУ, пока хоть у кого-то скопилось больше мегабайта
        # неразобранного (clientsAreBehind). Ждать мира, не читая второе
        # подключение, значит ждать его вечно.
        while True:
            message = session.probe.recv(0.05)
            if message is None:
                break
            session.take(message)
        message = control.recv(0.2)
        if message is not None and message.get("type") == "world_init":
            print("Отворачивание работает: тишина, пока отвернулся, "
                  "и мир целиком по возвращении")
            return 0
    print("Вернувшемуся клиенту не пришёл world_init")
    return 1


def main():
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-animal-delta-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT

    # Мир нарочно маленький и неторопливый, и это не экономия времени, а
    # условие работоспособности проверки.
    #
    # Наблюдатель здесь не просто читает — он ещё и applies каждую дельту к
    # своему списку, то есть работает больше клиента. Если он не успевает за
    # рассылкой, отставание не рассасывается: сервер продолжает слать, очередь
    # растёт, и — вот это главное — сервер перестаёт ЧИТАТЬ от него команды.
    # Поток соединения занят отправкой. Проверено прямо: при полноразмерном
    # мире с tick_interval_ms=1 команда toggle_pause не появлялась в журнале
    # сервера вовсе, мир уходил на полторы тысячи тиков вперёд, а проверка
    # объявляла "мир не встал на паузу" — хотя паузы никто и не просил, потому
    # что просьбу не прочитали.
    #
    # Размер Области при этом на суть проверки не влияет: дельта — про учёт
    # номеров в списке, рождения и смерти, а не про то, сколько на карте
    # клеток. Зато от размера напрямую зависит вес каждой дельты.
    config["area"] = {"width": 96, "height": 96}
    config["tick_interval_ms"] = 10
    # Реже, чем тик: копить изменения по нескольку тиков дешевле, чем слать
    # каждый. Дельта от этого не меняется — она описывает изменения, а не тики.
    config["snapshot_interval_ms"] = 100
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([server_binary, config_path], cwd=workdir,
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

        session.command({"type": "start_simulation"})
        for check in range(1, RESYNCS + 1):
            if not session.run_to(check * TICKS_BETWEEN):
                print(f"Мир не дошёл до тика {check * TICKS_BETWEEN}: "
                      f"остановился на {session.tick}")
                return 1

            # Мир на паузу: полный список приедет позже последней дельты, а
            # сравнивать надо одно и то же время. stop_simulation, а не
            # toggle_pause: просьба безусловная, и повторить её не страшно —
            # переключатель же зависит от того, каким сервер считает своё
            # состояние сейчас.
            session.command({"type": "stop_simulation"})

            # Дальше — тишина, и только по ней судим о паузе, а не по
            # сообщению pause_state. Оно приходит один раз, в момент смены
            # состояния, и поймать его обязан именно тот, кто в этот момент
            # слушает; захлебнувшемуся клиенту сервер перестаёт слать вовсе
            # (clientsAreBehind), и однажды пропущенное подтверждение
            # больше не повторится никогда. Тишина же проверяет то, что нас
            # действительно интересует: мир перестал меняться.
            #
            # Эта же тишина дочитывает хвост — дельту за последний тик,
            # которую рассылка не успела отправить до паузы (см. settle).
            if not session.settle():
                print("Мир не встал на паузу: сервер продолжает слать изменения")
                return 1

            # Собранное снимаем ДО того, как попросить полный мир: тот
            # придёт всем, в том числе слушающему подключению, и переселит
            # его список.
            collected = list(session.animals)
            collected_goblins = list(session.goblins)
            deltas = session.deltas

            # Полный список — по командному подключению (см. full_snapshot).
            # Прежде для этого открывалось ВТОРОЕ подключение: сервер шлёт
            # world_init всем, когда приходит новый наблюдатель. От него
            # пришлось отказаться — лишнее подключение к этому серверу
            # надёжно роняло существующее, recv получал WinError 10054 сразу
            # после первого же сличения, и до второго круга проверка не
            # доживала никогда. Воспроизводится и на сборке без гоблинов,
            # то есть к содержимому мира отношения не имеет.
            full_message = session.full_snapshot()
            if full_message is None:
                print("Сервер не прислал мир целиком")
                return 1
            if not full_message.get("paused", False):
                print("Мир не на паузе, сличать нечего: сервер прислал "
                      "мир с paused = false")
                return 1
            full = sorted(full_message.get("animals") or [], key=lambda a: a["id"])
            full_goblins = sorted(full_message.get("goblins") or [], key=lambda a: a["id"])
            if full_message.get("tick") != session.tick:
                print(f"Мир шагнул на паузе: дельты по тик {session.tick}, "
                      f"полный список на тик {full_message.get('tick')}")
                return 1
            if collected != full:
                report("животных", session.tick, collected, full)
                return 1
            if collected_goblins != full_goblins:
                report("гоблинов", session.tick, collected_goblins, full_goblins)
                return 1
            print(f"  сличение {check}/{RESYNCS} на тике {session.tick}: "
                  f"{len(full)} животных, {len(full_goblins)} гоблинов, "
                  f"{deltas} дельт позади — сходится")

            session.animals = full
            session.goblins = full_goblins
            session.deltas = 0
            session.command({"type": "toggle_pause"})

        print(f"Дельта существ сходится: {RESYNCS} сличений, тик {session.tick}")
        return check_suspension(session)
    finally:
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

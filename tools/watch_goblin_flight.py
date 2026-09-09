"""Убегает ли гоблин от хищника — числом, а не на глаз.

Вопрос, ради которого написана мера, задан так, чтобы на него можно было
ответить "нет":

    **Делает ли испуганный гоблин шаг, пока зубы стоят вплотную?**

Спрашивать надо именно так, а не "часто ли он называет страх". Желание он
называет исправно: при хищнике в двух клетках его выбирают девять из десяти.
Но желание — ещё не бегство, и замер до правки закона показал ровно
расхождение между ними: на высокогорье испуганный гоблин не делал шага НИ
РАЗУ за 2373 тика, а на голой земле уходил в 47.8% случаев. Он считал себя
спасённым везде, где зверю не встать, — и замирал на кромке полки, в одном
шаге от укуса (см. outOfReach, core/Climb.hpp).

Отсюда и устройство меры: доля шагающих считается ОТДЕЛЬНО по месту, где
гоблин стоит. Общее число не показывает ничего — оно смешивает того, кто
спасся, с тем, кто застрял, и оба выглядят одинаково неподвижными.

Три места, и они разные по сути:

* **дерево** — настоящее укрытие: хищник вычёркивает влезшего из списка
  добычи (outOfReachUpATree). Ноль шагов здесь — правильный ответ, а не
  изъян: гоблин сидит в кроне и пережидает;
* **высокогорье** — укрытие только в глубине. По кромке зверь ходит внизу и
  дотягивается на клетку, и ноль шагов здесь — та самая ошибка;
* **земля** — укрытия нет вовсе, и доля шагающих здесь есть мерка для двух
  остальных: столько уходит тот, кому больше ничего не остаётся.

Валун и плетень отдельной строкой не считаются: их на проводе нет, и оба
попадают в ту клетку, на которой стоят. Закон, однако, спрашивается о них
тем же самым (outOfReach), поэтому мера остаётся верной — она просто слепа
к тому, чтобы назвать их по имени.

Второй прогон для сравнения делать не нужно: числа сами по себе отвечают на
вопрос. Сравнивать имеет смысл, только если менялся закон бегства или
лазания, — тогда прогон до и прогон после, на одном seed.

Запуск:

    python3 tools/watch_goblin_flight.py 4000
"""
import collections
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server_binary import find_server
from ws_probe import WebSocketProbe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 9114
TICKS = 4000

# Потолок ноги (kLegCeiling = 8000, core/Climb.hpp) в тех долях, какими
# высота уходит на провод (kWireStep = 10, shared/protocol/WirePrecision.hpp).
LEG_CEILING = 800

# Докуда дотягивается удар (kStrikeReach = 1, core/Strike.hpp). Ровно на этом
# расстоянии вопрос "шагнул ли" и имеет смысл: дальше у гоблина ещё есть
# время, ближе — уже нет.
STRIKE_REACH = 1


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def apply_changes(creatures, changes):
    """То же самое, что делает NetworkClient::applyCreatureChanges.

    Списано из check_animal_delta.py, где оно машинно сверено с полным
    world_init: считать по своему разбору значило бы мерить не тот мир,
    который видит клиент.
    """
    triples = changes.get("pos", [])
    for p in range(0, len(triples) - 2, 3):
        creatures[triples[p]]["x"] = triples[p + 1]
        creatures[triples[p]]["y"] = triples[p + 2]
    for key in ("growth", "health", "desire", "fatigue", "carried", "material"):
        pairs = changes.get(key, [])
        for p in range(0, len(pairs) - 1, 2):
            creatures[pairs[p]][key] = pairs[p + 1]
    gone = set(changes.get("gone", []))
    if gone:
        creatures = [c for i, c in enumerate(creatures) if i not in gone]
    born = changes.get("born", [])
    if born:
        creatures = sorted(creatures + list(born), key=lambda c: c["id"])
    return creatures


def apply_layer(layer, pairs):
    """Слой в дельте — список пар "клетка, значение"."""
    for p in range(0, len(pairs) - 1, 2):
        layer[pairs[p]] = pairs[p + 1]


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-flight-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир и темп — как в остальных проверках, чтобы числа можно было класть
    # рядом с числами watch_goblin_deaths.py.
    config["area"] = {"width": 96, "height": 96}
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([server_binary, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        listen = connect()
        control = connect()
        if listen is None or control is None:
            print("Сервер не поднялся")
            return 1
        control.send({"type": "updates", "enabled": False})
        while control.recv(1.0) is not None:
            pass
        control.send({"type": "start_simulation"})

        goblins = None
        animals = None
        trees = None
        height = None
        width = 0
        tick = 0
        predators = []
        # Где гоблин стоит вообще и где стоит испуганный: без первого второе
        # не читается — поселение могло просто жить на камнях.
        anywhere = collections.Counter()
        scared = collections.Counter()
        # Шаг при зубах вплотную, по месту. Состояние берётся ПРОШЛОГО тика:
        # решение принималось там, а шаг виден здесь.
        close = collections.Counter()
        stepped = collections.Counter()
        previous = {}
        previous_tick = None

        while tick < ticks:
            message = listen.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                animals = sorted(message.get("animals") or [], key=lambda a: a["id"])
                width = message["area"]["width"]
                trees = list(message["layers"]["trees"])
                height = list(message["layers"]["height"])
                tick = message.get("tick", tick)
                continue
            if kind != "world_delta" or goblins is None:
                continue
            tick = message.get("tick", tick)

            # Слои — раньше существ: дерево могло вырасти или сгинуть в этом
            # же тике, и место гоблина считается по тому миру, в котором он
            # сейчас стоит.
            if "trees" in message:
                apply_layer(trees, message["trees"])
            if "height" in message:
                apply_layer(height, message["height"])
            if "animals" in message:
                animals = apply_changes(animals, message["animals"])
            if "goblins" in message:
                goblins = apply_changes(goblins, message["goblins"])

            predators = [(a["x"], a["y"]) for a in animals if a.get("kind") == "predator"]

            def spot(x, y):
                # Дерева на клетке нет — на проводе это -1: там едет номер
                # породы, а не признак (NetworkServer, слой "trees").
                cell = y * width + x
                if trees[cell] >= 0:
                    return "дерево"
                if height[cell] > LEG_CEILING:
                    return "высокогорье"
                return "земля"

            current = {}
            for goblin in goblins:
                where = spot(goblin["x"], goblin["y"])
                anywhere[where] += 1
                if goblin.get("desire") == "flee":
                    scared[where] += 1
                near = min((max(abs(px - goblin["x"]), abs(py - goblin["y"]))
                            for px, py in predators), default=None)
                current[goblin["id"]] = (goblin["x"], goblin["y"], near,
                                         goblin.get("desire", "?"), where)

            # Только соседние тики: между разорванными шаг посчитать нельзя,
            # а дельта могла и не прийти вовсе — мир не менялся.
            if previous_tick is not None and tick == previous_tick + 1:
                for name, (x, y, near, desire, where) in current.items():
                    was = previous.get(name)
                    if was is None:
                        continue
                    old_x, old_y, old_near, old_desire, old_where = was
                    if old_desire != "flee" or old_near is None or old_near > STRIKE_REACH:
                        continue
                    close[old_where] += 1
                    if (x, y) != (old_x, old_y):
                        stepped[old_where] += 1
            previous = current
            previous_tick = tick

        places = ("высокогорье", "дерево", "земля")
        everywhere = sum(anywhere.values()) or 1
        frightened = sum(scared.values()) or 1
        print(f"Тик {tick}, гоблинов {len(goblins)}, хищников {len(predators)}\n")
        print("Где гоблин стоит:")
        print(f"   {'место':>12} {'всегда':>8} {'при страхе':>12}")
        for where in places:
            print(f"   {where:>12} {anywhere[where] * 100 / everywhere:7.1f}%"
                  f" {scared[where] * 100 / frightened:11.1f}%")
        print("\nИспуганный, зубы вплотную — сделал ли шаг:")
        print(f"   {'место':>12} {'тиков':>8} {'шагнул':>8}")
        for where in places:
            total = close[where]
            if total == 0:
                print(f"   {where:>12} {0:8} {'—':>8}")
                continue
            print(f"   {where:>12} {total:8} {stepped[where] * 100 / total:7.1f}%")
        print("\nЧитать так: ноль на дереве — правильно (хищник вычёркивает"
              " влезшего),\nноль на высокогорье — застрявший, и мерка обоим —"
              " доля на земле.")
    finally:
        server.terminate()
        try:
            server.wait(10)
        except Exception:
            server.kill()
        shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

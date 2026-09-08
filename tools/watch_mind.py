"""Что меняет смена разума — числом, а не на глаз.

Разум отделён от законов мира (`core/Mind.hpp`) и сменен переключателем
`terrain.toggles.lottery_mind`. Разумов два:

* **greedy** — наибольший вес, при равенстве побеждает последний в списке.
  Ровно то, чем мир жил до появления шва.
* **lottery** — жребий по весам, вес идёт в квадрате.

Мера отвечает на три вопроса, и каждый задан так, чтобы на него можно было
ответить "нет".

1. **Шов ничего не сломал?** При `greedy` мир обязан вести себя как прежде:
   доли желаний, поголовье и смерти — те же. Это проверка не разума, а самой
   правки, и она первая, потому что при "нет" остальное неважно.

2. **Жребий вообще что-то меняет?** Доли желаний при `lottery` обязаны стать
   ровнее: сильное желание остаётся частым, но перестаёт быть единственным.
   Не стали — квадрат веса слишком крут, и жребий выродился в жадность.

3. **Мир это переживает?** Поголовье и смерти того же ПОРЯДКА (CLAUDE.md:
   расхождение в разы — повод разбираться, а не строчка в отчёте). Разум,
   при котором зверьё вымирает, — не разум, а поломка.

Замер — ПАРА прогонов одного бинарника на одном seed:

    python3 tools/watch_mind.py 12000 greedy
    python3 tools/watch_mind.py 12000 lottery

Двумя сборками мерить нельзя — сравнивалось бы заодно всё, что успело
измениться в ядре между ними. Ровно ради этого разум и сделан переключателем
мира, а не выбором сборки.
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
PORT = 9117
TICKS = 12000


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def apply_changes(creatures, changes):
    """То же самое, что делает NetworkClient::applyCreatureChanges."""
    triples = changes.get("pos", [])
    for p in range(0, len(triples) - 2, 3):
        creatures[triples[p]]["x"] = triples[p + 1]
        creatures[triples[p]]["y"] = triples[p + 2]
    for key in ("growth", "health", "desire", "fatigue", "carried", "material"):
        pairs = changes.get(key, [])
        for p in range(0, len(pairs) - 1, 2):
            creatures[pairs[p]][key] = pairs[p + 1]
    gone = set(changes.get("gone", []))
    dead = [c for i, c in enumerate(creatures) if i in gone]
    if gone:
        creatures = [c for i, c in enumerate(creatures) if i not in gone]
    born = changes.get("born", [])
    if born:
        creatures = sorted(creatures + list(born), key=lambda c: c["id"])
    return creatures, dead, len(born)


def evenness(counts):
    """Насколько ровно разошлись доли: 0 — всё в одном желании, 1 — поровну.

    Считается как отношение "сколько видов желаний набрало бы столько же при
    равных долях" к их числу — та же мысль, что у эффективного числа видов в
    экологии, только без логарифмов: доля в квадрате, сумма, обратное.

    Число это нужно ровно затем, чтобы ответить на второй вопрос меры одной
    величиной, а не глазами по столбику процентов.
    """
    total = sum(counts.values())
    if total <= 0 or len(counts) <= 1:
        return 0.0
    shares = [v / total for v in counts.values()]
    concentration = sum(s * s for s in shares)
    effective = 1.0 / concentration if concentration > 0 else 0.0
    return effective / len(counts)


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    mind = sys.argv[2] if len(sys.argv) > 2 else "greedy"
    if mind not in ("greedy", "lottery"):
        print("Разум бывает greedy или lottery")
        return 1
    binary = find_server(ROOT)
    if binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-mind-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    config["area"] = {"width": 96, "height": 96}
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
    # Разум переключается ЗДЕСЬ, а не пересборкой: один бинарник, два прогона,
    # разница в одном поле.
    config.setdefault("terrain", {}).setdefault("toggles", {})["lottery_mind"] = mind == "lottery"
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([binary, config_path], cwd=workdir,
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
        tick = 0
        goblinWants = collections.Counter()
        animalWants = collections.Counter()
        goblinDeaths = 0
        animalDeaths = 0
        goblinBorn = 0
        goblinSeen = 0
        animalSeen = 0
        while tick < ticks:
            message = listen.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                if max(message["layers"]["rockiness"]) <= 0:
                    continue  # первый снимок приходит до генерации
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                animals = sorted(message.get("animals") or [], key=lambda a: a["id"])
                tick = message.get("tick", tick)
                continue
            if kind != "world_delta" or goblins is None:
                continue
            tick = message.get("tick", tick)
            if "goblins" in message:
                goblins, dead, born = apply_changes(goblins, message["goblins"])
                goblinDeaths += len(dead)
                goblinBorn += born
            if "animals" in message:
                animals, dead, _ = apply_changes(animals, message["animals"])
                animalDeaths += len(dead)
            for g in goblins:
                goblinWants[g.get("desire", "?")] += 1
                goblinSeen += 1
            for a in animals:
                animalWants[a.get("desire", "?")] += 1
                animalSeen += 1

        control.send({"type": "stop_simulation"})

        print(f"Разум: {mind}, тиков {tick}")
        print(f"Поголовье в среднем: гоблинов {goblinSeen / max(1, tick):.1f}, "
              f"зверья {animalSeen / max(1, tick):.1f}")
        print(f"Смертей: гоблинов {goblinDeaths}, зверья {animalDeaths}; рождений гоблинов {goblinBorn}")
        for name, wants, seen in (("гоблин", goblinWants, goblinSeen), ("зверь", animalWants, animalSeen)):
            if seen <= 0:
                continue
            print(f"желания ({name}), ровность {evenness(wants):.2f}:")
            for want, count in wants.most_common():
                print(f"  {want:8s} {100.0 * count / seen:5.1f}%")
        return 0
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

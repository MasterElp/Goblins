"""Проверка круга сохранения: доезжает ли мир через файл без потерь.

Круг такой: мир -> save_world -> файл -> start_simulation с именем -> мир.
Потерянное на любом переходе ведёт себя одинаково скверно и одинаково
незаметно: мир открывается, выглядит миром, живёт дальше — просто это уже не
тот мир, который сохраняли.

Сохранение перечисляет компоненты ПОИМЁННО (server/WorldSave.cpp), и в этом
всё дело: новое свойство мира не попадает в файл само. Так и вышло с
племенами гоблинов — гоблины сохранялись и загружались, а племена нет, и
загруженный мир оставался с населением, но без архетипов: мутациям стало не
вокруг чего гулять (kSpeciesBand), и за поколения племена слились бы в одно.
Числа при этом сходились, поголовье было на месте, и увидеть потерю было
нечем.

Сличается world_init, пришедший В МОМЕНТ ЗАГРУЗКИ, а не снятый позже:
загрузка снимает паузу, и мир успевает шагнуть. Сравнивать его с сохранённым
через несколько тиков значит сравнивать не файл, а время — существа честно
разойдутся на клетку, и проверка объявит это потерей.

Что сверяется: тик, списки существ поимённо (карточка в карточку), племена и
виды с их геномами, и все тайловые слои целиком.

Запуск (сервер поднимается сам):
    python3 tools/check_world_save.py
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
PORT = 9108
WORLD = "check-world-save"
# Сколько прожить до сохранения. Достаточно, чтобы кто-нибудь родился и
# умер, — сохранять только что созданный мир значит не проверить ничего.
TICKS = 300


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def wait_for(probe, kind, seconds=120):
    deadline = time.time() + seconds
    while True:
        left = deadline - time.time()
        if left <= 0:
            return None
        message = probe.recv(left)
        if message is None:
            return None
        if message.get("type") == kind:
            return message


def settle(probe, seconds=20.0, quiet=1.0):
    """Дочитать всё и дождаться тишины. Только на остановленном мире."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if probe.recv(quiet) is None:
            return True
    return False


def watch_goblin(session_probe, control, goblin_id, seconds=30):
    """Подробности одного гоблина — то, чего нет в общем списке.

    Память места (`knows`) не едет в карточке и в дельте: восемь мест на
    каждого каждый тик — это дорого, а нужны они по одному, у того, за кем
    следят (см. buildWatchedJson). Значит и проверить её сохранность иначе
    нельзя: пропади она при загрузке, общий список сойдётся до последнего
    числа, а гоблины откроют мир, забывшими всё, и начнут набивать свои
    тропы заново.
    """
    # Сперва отпустить прежнюю цель. Сервер шлёт "watched" только когда она
    # ИЗМЕНИЛАСЬ, и повторная просьба следить за тем же самым не меняет
    # ничего — ответа на неё не будет никогда.
    control.send({"type": "watch", "kind": "none", "id": 0, "x": 0, "y": 0})
    while session_probe.recv(0.3) is not None:
        pass
    control.send({"type": "watch", "kind": "goblin", "id": goblin_id, "x": 0, "y": 0})
    deadline = time.time() + seconds
    while time.time() < deadline:
        message = session_probe.recv(min(5.0, max(0.1, deadline - time.time())))
        if message is None:
            continue
        watched = message.get("watched")
        if isinstance(watched, dict) and watched.get("id") == goblin_id:
            return watched
    return None


def compare_memory(goblin_id, before, after):
    """Что должно совпасть в памяти, а что совпасть не может.

    Места — точно: клетка, вид, их число и порядок. Потеряйся хоть одно, и
    гоблин открыл бы мир, забывшим дорогу, которую набивал.

    А вот твёрдость точно совпасть НЕ МОЖЕТ, и требовать этого было бы
    требованием остановить время. Между тем, как мир записан в файл, и тем,
    как он прочитан обратно, проходят тики: загрузка снимает паузу, и
    забывание работает каждый тик (core/Knowledge.hpp). Поэтому от твёрдости
    спрашивается ровно то, что отличает сохранённую память от несохранённой:
    она на месте (не ноль) и не выросла — убыть за это время она могла, а
    прибавиться неоткуда.
    """
    if not after:
        return [f"память гоблина {goblin_id} пропала целиком: было {before}"]
    if len(before) != len(after):
        return [f"память гоблина {goblin_id}: было {len(before)} мест, стало {len(after)}"]
    problems = []
    for was, now in zip(before, after):
        if (was["x"], was["y"], was["kind"]) != (now["x"], now["y"], now["kind"]):
            problems.append(f"память гоблина {goblin_id}: место {was} стало {now}")
        elif now["strength"] <= 0 or now["strength"] > was["strength"]:
            problems.append(f"память гоблина {goblin_id}: твёрдость {was['strength']} стала "
                            f"{now['strength']} (могла только убыть и не до нуля)")
    return problems


def compare(before, after):
    """Что именно разошлось. Пусто — не разошлось ничего."""
    problems = []
    if before.get("tick") != after.get("tick"):
        problems.append(f"тик: было {before.get('tick')}, стало {after.get('tick')}")

    for key in ("animals", "goblins"):
        old = sorted(before.get(key) or [], key=lambda c: c["id"])
        new = sorted(after.get(key) or [], key=lambda c: c["id"])
        if len(old) != len(new):
            problems.append(f"{key}: было {len(old)}, стало {len(new)}")
            continue
        wrong = [(a, b) for a, b in zip(old, new) if a != b]
        if wrong:
            problems.append(f"{key}: {len(wrong)} карточек из {len(old)} не совпали")
            for a, b in wrong[:2]:
                problems.append(f"    до:    {a}")
                problems.append(f"    после: {b}")

    # Виды и племена — свойство мира, и теряются они тише всего: население
    # на месте, а расти ему больше не вокруг чего.
    for key in ("animal_species", "goblin_tribes", "plant_species", "tree_species"):
        if before.get(key) != after.get(key):
            problems.append(f"{key}: разошлись")

    old_layers = before.get("layers") or {}
    new_layers = after.get("layers") or {}
    for name in sorted(set(old_layers) | set(new_layers)):
        if old_layers.get(name) != new_layers.get(name):
            problems.append(f"слой {name}: разошёлся")
    return problems


def main():
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-world-save-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир маленький и неторопливый по той же причине, что и в
    # check_animal_delta.py: сервер не читает команд с того подключения,
    # которое сам заливает, и разговор с ним должен оставаться разговором.
    config["area"] = {"width": 96, "height": 96}
    config["tick_interval_ms"] = 10
    config["snapshot_interval_ms"] = 100
    config["saves_dir"] = "saves"
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
        # Командное подключение отворачивается сразу и навсегда: по залитому
        # подключению сервер команд не читает (см. check_animal_delta.py).
        control.send({"type": "updates", "enabled": False})
        while control.recv(1.0) is not None:
            pass

        control.send({"type": "start_simulation"})
        tick = 0
        while tick < TICKS:
            message = listen.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            if message.get("type") in ("world_init", "world_delta"):
                tick = message.get("tick", tick)

        control.send({"type": "stop_simulation"})
        if not settle(listen):
            print("Мир не встал на паузу")
            return 1

        # Мир целиком, каким он уходит в файл. Просит командное подключение,
        # читает слушающее (тот же приём, что в check_animal_delta.py).
        control.send({"type": "updates", "enabled": True})
        control.send({"type": "updates", "enabled": False})
        before = wait_for(listen, "world_init")
        while control.recv(0.2) is not None:
            pass
        if before is None:
            print("Сервер не прислал мир целиком до сохранения")
            return 1

        control.send({"type": "save_world", "name": WORLD})
        notice = wait_for(listen, "notice")
        if notice is None or "saved" not in notice.get("text", ""):
            print(f"Мир не сохранился: {None if notice is None else notice.get('text')}")
            return 1

        # Загрузка присылает мир целиком САМА, на сохранённом тике, — его и
        # сличаем. Снимать состояние позже нельзя: загрузка снимает паузу, и
        # разошлись бы не файл со снимком, а два разных момента времени.
        control.send({"type": "start_simulation", "world": WORLD})
        after = wait_for(listen, "world_init")
        # Загрузка снимает паузу — останавливаем мир сразу, как только он
        # пришёл, чтобы он ушёл от сохранённого тика как можно меньше.
        control.send({"type": "stop_simulation"})
        settle(listen)
        if after is None:
            print("Загруженный мир не пришёл")
            return 1

        problems = compare(before, after)

        # Память — отдельной проверкой, по одному гоблину: в общем списке её
        # нет. Берём того, кто успел что-то запомнить; если такого нет вовсе,
        # проверять нечего и молчать об этом нельзя.
        remembered_before = None
        watched_id = None
        for goblin in before.get("goblins") or []:
            watched = watch_goblin(listen, control, goblin["id"])
            if watched and watched.get("knows"):
                remembered_before = watched["knows"]
                watched_id = goblin["id"]
                break
        if watched_id is None:
            problems.append("ни один гоблин ничего не помнит — память проверить не на чем")
        else:
            remembered_after = watch_goblin(listen, control, watched_id)
            got = None if remembered_after is None else remembered_after.get("knows")
            problems.extend(compare_memory(watched_id, remembered_before, got))
        counts = (len(before.get("animals") or []), len(before.get("goblins") or []))
        if problems:
            print(f"ПОТЕРЯ при сохранении (тик {before.get('tick')}):")
            for problem in problems:
                print(f"  - {problem}")
            return 1
        print(f"Круг сохранения замкнут: тик {before.get('tick')}, "
              f"{counts[0]} животных, {counts[1]} гоблинов — мир доезжает через файл без потерь")
        return 0
    finally:
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

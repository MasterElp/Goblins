"""Строится ли забор — и огораживает ли он хоть что-нибудь.

СЕЙЧАС ЭТА МЕРА ПОКАЗЫВАЕТ НУЛИ, И ЭТО ПРАВИЛЬНЫЙ ОТВЕТ. Желание городиться
убрано из головы гоблина (docs/10_Goblins.md, "Забор: вещь в мире, которую
пока никто не хочет"): плетень остался вещью мира, но ставить его некому.
Мера оставлена целой — она понадобится в тот же день, когда желание заведут
заново, и покажет не "сколько кольев", а "заперли ли".

Поголовье хищников печатается рядом с прочностью плетня нарочно: порознь эти
дорожки врут. В мире без зубов ноль заборов — правильный ответ, а не поломка.

Запирающая прочность — kFenceShut (core/Bound.hpp), половина полной. Клетка
слабее не держит ногу зверя, то есть забором ещё не является.

    python3 tools/watch_fence.py [тиков]

Переменные окружения: GOBLINS_W, GOBLINS_H — размер Области (по умолчанию из
config.json), GOBLINS_PRED, GOBLINS_HERB — поголовье при рождении мира (стенд,
где зубы держатся дольше), GOBLINS_BASE — корень другой сборки (сличение с
прежним ядром), GOBLINS_PORT.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from server_binary import find_server
from ws_probe import WebSocketProbe

PORT = int(os.environ.get("GOBLINS_PORT", "9171"))
# Прочность едет на провод сотыми (shared/protocol/WirePrecision.hpp), а
# kFenceShut = 500 из тысячи.
SHUT_WIRE = 50
SITE_FENCE = 3  # BuildKind::Fence в слое площадок
K_LEG_CEILING_WIRE = 800  # kLegCeiling = 8000, на проводе сотые
# Какую землю считать обжитой при подсчёте огороженного. Это мерка САМОЙ
# МЕРЫ, а не закон мира: в мире такого порога нет (см. core/Bound.hpp —
# край обжитого ушёл вместе с решением гоблина городиться). Половина
# полной утоптанности отделяет лагерь от случайного следа.
K_LIVED_IN_WIRE = 50
DIRS = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]


def walledIn(trampled, fence, water, height, w, h):
    """Сколько обжитой земли заперто ИМЕННО ПЛЕТНЁМ.

    Заливка считается дважды — с забором и без него, — и печатается разница.
    Без вычитания мера врёт: замер на мире, где не стоит ни одной запертой
    клетки, дал 64 клетки "за оградой" — это закутки рельефа, вода да скалы,
    куда зверю не дойти и без гоблинов.
    """
    zero = [0] * len(fence)
    return enclosed(trampled, fence, water, height, w, h) -         enclosed(trampled, zero, water, height, w, h)


def enclosed(trampled, fence, water, height, w, h):
    """Утоптанные клетки, до которых ЗВЕРЬ не доберётся от края карты.

    Прямой ответ на вопрос "огородили ли", и он не зависит от того, как легли
    колья: заливка идёт по тем же клеткам, по которым может ступить зверь
    (core/Path.hpp, standableAt при kOnLegs), от края карты внутрь. Что
    осталось непройденным и при этом обжито — то и за оградой.

    Считать длиной стены или числом кольев этого нельзя: сто клеток плетня,
    разложенных как попало, не запирают ничего, а тридцать, сомкнутые в
    кольцо, запирают.
    """
    def passable(i):
        return water[i] <= 0 and height[i] <= K_LEG_CEILING_WIRE and fence[i] < SHUT_WIRE

    seen = bytearray(w * h)
    stack = []
    for x in range(w):
        for y in (0, h - 1):
            i = y * w + x
            if not seen[i] and passable(i):
                seen[i] = 1
                stack.append(i)
    for y in range(h):
        for x in (0, w - 1):
            i = y * w + x
            if not seen[i] and passable(i):
                seen[i] = 1
                stack.append(i)
    while stack:
        i = stack.pop()
        x, y = i % w, i // w
        for dx, dy in DIRS:
            nx, ny = x + dx, y + dy
            if not (0 <= nx < w and 0 <= ny < h):
                continue
            j = ny * w + nx
            if not seen[j] and passable(j):
                seen[j] = 1
                stack.append(j)
    return sum(1 for i in range(w * h)
               if not seen[i] and trampled[i] >= K_LIVED_IN_WIRE and water[i] <= 0)


def longest_wall(fence, w, h):
    """Самый длинный связный кусок ЗАПЕРТОГО плетня (8 соседей).

    Отличает растущую молнию от сыпи ещё до того, как кольцо сомкнулось: это
    единственное, что видно раньше замкнутости.
    """
    cells = {i for i, v in enumerate(fence) if v >= SHUT_WIRE}
    seen = set()
    best = 0
    for start in cells:
        if start in seen:
            continue
        stack = [start]
        seen.add(start)
        size = 0
        while stack:
            i = stack.pop()
            size += 1
            x, y = i % w, i // w
            for dx, dy in DIRS:
                nx, ny = x + dx, y + dy
                if not (0 <= nx < w and 0 <= ny < h):
                    continue
                j = ny * w + nx
                if j in cells and j not in seen:
                    seen.add(j)
                    stack.append(j)
        best = max(best, size)
    return best


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else 12000
    binary = find_server(os.environ.get("GOBLINS_BASE", ROOT))
    if binary is None:
        print("сервер не собран")
        return 1
    workdir = tempfile.mkdtemp(prefix="goblins-fence-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    if os.environ.get("GOBLINS_W") and os.environ.get("GOBLINS_H"):
        config["area"] = {"width": int(os.environ["GOBLINS_W"]), "height": int(os.environ["GOBLINS_H"])}
    for key, name in (("GOBLINS_PRED", "predator_count"), ("GOBLINS_HERB", "herbivore_count")):
        if os.environ.get(key):
            config["animals"][name] = int(os.environ[key])
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
    path = os.path.join(workdir, "config.json")
    json.dump(config, open(path, "w", encoding="utf-8"), indent=4, sort_keys=True)
    server = subprocess.Popen([binary, path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        listen, control = connect(), connect()
        if listen is None or control is None:
            print("сервер не поднялся")
            return 1
        control.send({"type": "updates", "enabled": False})
        while control.recv(1.0) is not None:
            pass
        control.send({"type": "start_simulation"})

        fence = site = None
        trampled = water = height = None
        w = h = 0
        animals = []
        tick, mark = 0, 2000
        sites_seen = 0
        while tick < ticks:
            m = listen.recv(120)
            if m is None:
                print(f"мир встал на тике {tick}")
                return 1
            kind = m.get("type")
            if kind == "world_init":
                # Первый снимок приходит до рождения рельефа — по нему считать
                # нечего.
                if max(m["layers"]["rockiness"]) <= 0:
                    continue
                w = m["area"]["width"]
                h = m["area"]["height"]
                fence = list(m["layers"].get("fence") or [0] * len(m["layers"]["canopy"]))
                site = list(m["layers"]["site"])
                trampled = list(m["layers"]["trampled"])
                water = list(m["layers"]["water"])
                height = list(m["layers"]["height"])
                animals = list(m.get("animals", []))
                tick = m.get("tick", tick)
                continue
            if kind != "world_delta" or fence is None:
                continue
            tick = m.get("tick", tick)
            # В дельте слои лежат на ВЕРХНЕМ уровне сообщения, а не под
            # "layers" — так их читает и клиент (applyChangedCells).
            for name, arr in (("fence", fence), ("site", site), ("trampled", trampled),
                               ("water", water), ("height", height)):
                pairs = m.get(name) or []
                for i in range(0, len(pairs) - 1, 2):
                    arr[pairs[i]] = pairs[i + 1]
            changes = m.get("animals")
            if isinstance(changes, dict):
                gone = set(changes.get("gone", []))
                if gone:
                    animals = [a for i, a in enumerate(animals) if i not in gone]
                born = changes.get("born", [])
                if born:
                    animals = sorted(animals + list(born), key=lambda a: a["id"])
            sites_seen = max(sites_seen, sum(1 for v in site if v == SITE_FENCE))
            if tick >= mark:
                predators = sum(1 for a in animals if a.get("kind") == "predator")
                woven = [v for v in fence if v > 0]
                print(f"тик {tick}: хищников {predators}, кольев разом {sum(1 for v in site if v == SITE_FENCE)}, "
                      f"клеток с плетнём {len(woven)}, лучшая {max(woven, default=0)}/100, "
                      f"запертых {sum(1 for v in woven if v >= SHUT_WIRE)}, "
                      f"стена подряд {longest_wall(fence, w, h)}, "
                      f"заперто плетнём {walledIn(trampled, fence, water, height, w, h)}")
                mark += 2000

        control.send({"type": "stop_simulation"})
        woven = [v for v in fence if v > 0]
        predators = sum(1 for a in animals if a.get("kind") == "predator")
        print()
        inside = walledIn(trampled, fence, water, height, w, h)
        print(f"За {tick} тиков: площадок под забор было видно разом до {sites_seen}, "
              f"клеток с плетнём {len(woven)}, запертых {sum(1 for v in woven if v >= SHUT_WIRE)}")
        print(f"Самый длинный сплошной кусок стены: {longest_wall(fence, w, h)} клеток")
        print(f"ОБЖИТОЙ ЗЕМЛИ ЗАПЕРТО ПЛЕТНЁМ (сверх того, что запирает сам рельеф): {inside} клеток")
        if predators == 0:
            print("Хищников к концу не осталось — забору больше не от кого стоять, "
                  "и работа по нему прекращается вместе с ними.")
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

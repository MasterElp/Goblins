"""Даёт ли лазание убежище — числом, а не на глаз.

Высота — первая преграда в этом мире, которая РАЗЛИЧАЕТ идущих: выше
kLegCeiling зверь не забирается, гоблин забирается (core/Climb.hpp).
Проверить это глазами нельзя: на карте видно, где кто стоит, но не видно,
куда он не смог пойти.

Четыре вопроса, и каждый задан так, чтобы на него можно было ответить "нет".

1. **Не заперт ли зверь?** Земля ниже потолка обязана оставаться ОДНИМ
   связным куском. Разрежь её потолок надвое — и убежище гоблину куплено
   ценой загонов для всех остальных. Это первый вопрос, потому что при "нет"
   всё остальное неважно.

2. **Есть ли вообще убежище?** Доля суши выше потолка ноги. Ноль означает,
   что лезть в этом мире некуда: мир вышел холмистым, гор в нём нет. Число
   это сильно разное от мира к миру — свойство миров, а не изъян закона.

3. **Пользуются ли им?** Доля гоблинов, стоящих там, куда зверю хода нет, —
   ПОРОЗНЬ у спокойных и у испуганных. Если у испуганных она не выше, значит
   вверх никто не бежит, и закон бегства не работает, даже если убежище есть.

4. **Спасает ли оно?** Поголовье и частота смертей. Доля смертей от зубов
   отвечает на вопрос "отчего умирают", но не на вопрос "спасает ли": при
   вдвое большем поголовье та же доля означает вдвое меньшую опасность для
   каждого.

Запуск:

    python3 tools/watch_climb.py 6000

Пары прогонов здесь не нужно, в отличие от нрава и троп: закон включается не
настройкой мира, а самим наличием гор, и сравнивать надо не два мира, а
спокойных с испуганными внутри одного.
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
PORT = 9115
TICKS = 6000

# Потолки из core/Climb.hpp. Повторены здесь числами: на провод они не
# уезжают, а без них мера не может сказать, где кончается зверь и начинается
# гоблин. Разойдутся — соврут все четыре ответа, поэтому имена те же.
LEG_CEILING = 8000
HAND_CEILING = 20000

# Высота на проводе идёт сотыми (shared/protocol/WirePrecision.hpp), а в
# законе числа мировые — тысячные. Одно деление, но забыть его значит
# ошибиться в десять раз, а диапазоны здесь как раз десятикратные.
WIRE_STEP = 10


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
    gone_indices = set(changes.get("gone", []))
    gone = [c for i, c in enumerate(creatures) if i in gone_indices]
    if gone_indices:
        creatures = [c for i, c in enumerate(creatures) if i not in gone_indices]
    born = changes.get("born", [])
    if born:
        creatures = sorted(creatures + list(born), key=lambda c: c["id"])
    return creatures, gone


def largest_piece(w, h, ok):
    """Наибольший связный кусок среди помеченных клеток. По восьми соседям,
    как и ходьба."""
    seen = bytearray(w * h)
    best = 0
    for y0 in range(h):
        for x0 in range(w):
            i0 = y0 * w + x0
            if seen[i0] or not ok[i0]:
                continue
            size = 0
            queue = collections.deque([(x0, y0)])
            seen[i0] = 1
            while queue:
                x, y = queue.popleft()
                size += 1
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        nx, ny = x + dx, y + dy
                        if not (0 <= nx < w and 0 <= ny < h):
                            continue
                        j = ny * w + nx
                        if seen[j] or not ok[j]:
                            continue
                        seen[j] = 1
                        queue.append((nx, ny))
            best = max(best, size)
    return best


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-climb-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
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
        width = height_cells = 0
        legs = hands = None
        land = 0
        tick = 0
        # Сколько раз гоблин был замечен наверху — порознь у спокойных и у
        # испуганных. ПОРОЗНЬ, а не "все и испуганные": испуганные входят во
        # "всех", и такое сравнение сравнивало бы множество с самим собой —
        # доли совпали бы тем точнее, чем больше в мире испуга.
        calm_total = calm_up = 0
        scared_total = scared_up = 0
        deaths = []
        worst_drop = collections.Counter()
        while tick < ticks:
            message = listen.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                # Первый world_init приходит ДО генерации и пуст: нули во
                # всех слоях. Мир рождается по команде start_simulation.
                if max(message["layers"]["rockiness"]) <= 0:
                    continue
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                width = message["area"]["width"]
                height_cells = message["area"]["height"]
                relief = [v * WIRE_STEP for v in message["layers"]["height"]]
                water = message["layers"]["water"]
                cells = width * height_cells
                dry = [1 if water[i] <= 0 else 0 for i in range(cells)]
                land = sum(dry)
                legs = [1 if (dry[i] and relief[i] <= LEG_CEILING) else 0 for i in range(cells)]
                hands = [1 if (dry[i] and relief[i] <= HAND_CEILING) else 0 for i in range(cells)]
                byLeg = sum(legs)
                byHand = sum(hands)
                refuge = byHand - byLeg
                # --- 1. Не заперт ли зверь.
                piece = largest_piece(width, height_cells, legs)
                verdict = "один кусок" if piece >= byLeg * 0.98 else "ЗВЕРЬ ЗАПЕРТ"
                print(f"Суша {land} клеток, высота {min(relief)}..{max(relief)}.")
                print(f"Ногой доступно {byLeg} ({100.0*byLeg/land:.1f}% суши), "
                      f"наибольший связный кусок {piece} "
                      f"({100.0*piece/max(1,byLeg):.1f}% от них) — {verdict}")
                # --- 2. Есть ли убежище.
                print(f"Убежище (рукой можно, ногой нет): {refuge} клеток "
                      f"({100.0*refuge/land:.1f}% суши)")
                tick = message.get("tick", tick)
                continue
            if kind != "world_delta" or goblins is None or legs is None:
                continue
            tick = message.get("tick", tick)
            changes = message.get("goblins")
            if changes:
                before = {g["id"]: g.get("health", 0) for g in goblins}
                goblins, gone = apply_changes(goblins, changes)
                for g in goblins:
                    was = before.get(g["id"])
                    now = g.get("health", 0)
                    if was is not None and was - now > worst_drop[g["id"]]:
                        worst_drop[g["id"]] = was - now
                for dead in gone:
                    was = before.get(dead["id"], dead.get("health", 0))
                    drop = max(worst_drop[dead["id"]], was - dead.get("health", 0))
                    deaths.append("зубы" if drop >= 10 else "иное")
            # --- 3. Пользуются ли убежищем.
            for g in goblins:
                i = g["y"] * width + g["x"]
                if not (0 <= i < width * height_cells):
                    continue
                up = hands[i] and not legs[i]
                if g.get("desire") == "flee":
                    scared_total += 1
                    scared_up += 1 if up else 0
                else:
                    calm_total += 1
                    calm_up += 1 if up else 0

        control.send({"type": "stop_simulation"})

        if calm_total + scared_total == 0:
            print("Гоблинов на карте не было — мерить нечего")
            return 1
        calm_share = 100.0 * calm_up / calm_total if calm_total else 0.0
        print(f"В убежище стоял СПОКОЙНЫЙ гоблин: {calm_share:.1f}% "
              f"({calm_total} наблюдений)")
        if scared_total == 0:
            print("Испуганным не был никто: зубы до поселения не дошли")
        else:
            scared_share = 100.0 * scared_up / scared_total
            verdict = "вверх бегут" if scared_share > calm_share + 1.0 else "ВВЕРХ НИКТО НЕ БЕЖИТ"
            print(f"В убежище стоял ИСПУГАННЫЙ: {scared_share:.1f}% "
                  f"({scared_total} наблюдений) — {verdict}")
        # Поголовье и ЧАСТОТА смертей, а не одна их доля. Доля отвечает на
        # вопрос "отчего умирают", но не на вопрос "спасает ли убежище": при
        # вдвое большем поголовье та же доля означает вдвое меньшую опасность
        # для каждого. Сравнивать пару прогонов надо по этим двум числам.
        lived = calm_total + scared_total
        print(f"Поголовье в среднем: {lived / max(1, ticks):.1f} гоблинов")
        if deaths:
            teeth = 100.0 * sum(1 for d in deaths if d == "зубы") / len(deaths)
            print(f"Смертей {len(deaths)}, от зубов {teeth:.0f}%; "
                  f"на тысячу гоблино-тиков {1000.0 * len(deaths) / max(1, lived):.3f}")
        else:
            print("За прогон не умер никто")
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

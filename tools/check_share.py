"""Проверка дележа: делимое обязано расходиться.

Спор за одну клетку решается долями (`core/Share.hpp`): если желаемого
меньше, чем просят, каждый получает свою часть. Проверяется здесь ровно одно
следствие этого закона — **того, что лежит на клетке, не может не достаться
никому.**

Звучит как тавтология, но именно это и сломалось однажды, молча и надолго.
Доля считалась от полного наличия и округлялась вниз: `want * available /
demand`. На тысячных (трава, мясо) это почти никогда не давало нуля, а на
штучном дало сразу. Две ягоды на кусте, трое просящих, каждый просит две:
`2 * 2 / 6 = 0`. Ноль всем. Куст оставался нетронутым, а трое голодных стояли
на нём **вечно** — еда под ногами, взять нельзя, уйти незачем: голод не
проходит, а желание держится инерцией. Замер поймал стоянки по две-три тысячи
тиков.

Увидеть это в клиенте было нечем. Гоблин честно стоял на кусте, панель
честно писала "picking berries", счётчик ягод честно показывал 2 — и всё это
было правдой. Неправдой было только то, чего не показывает никто: что за две
тысячи тиков он не сорвал ни одной.

Отсюда и форма проверки: следим за клетками с ягодами и за теми, кто на них
стоит. Простоял долго, а счёт ягод под ним не сдвинулся ни разу — дележ
выродился.

Ягоды взяты не потому, что они важнее травы, а потому, что они **штучные**:
на них вырождение видно первым. Сломайся закон снова — здесь и проявится.

Запуск (сервер поднимается сам):
    python3 tools/check_share.py [тиков]
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
PORT = 9112
TICKS = 3000

# Сколько тиков простоя на неубывающем кусте считать зависанием.
#
# Двести — заведомо больше всего, что бывает при исправном законе: полный
# куст (kBerryMax = 12) обирается за считанные тики, и даже спор десятерых за
# одну ягоду разрешается за десяток. И заведомо меньше того, что даёт
# поломка: там простой длится до конца прогона, сколько бы тот ни шёл.
STUCK = 200


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def apply_changes(goblins, changes):
    """Тот же разбор дельты, каким её применяет клиент (applyCreatureChanges)."""
    triples = changes.get("pos", [])
    for i in range(0, len(triples) - 2, 3):
        goblins[triples[i]]["x"] = triples[i + 1]
        goblins[triples[i]]["y"] = triples[i + 2]
    for key in ("growth", "health", "desire", "fatigue", "carried", "material"):
        pairs = changes.get(key, [])
        for p in range(0, len(pairs) - 1, 2):
            goblins[pairs[p]][key] = pairs[p + 1]
    for index in sorted(changes.get("gone", []), reverse=True):
        if 0 <= index < len(goblins):
            del goblins[index]
    for born in changes.get("born", []):
        goblins.append(born)
    goblins.sort(key=lambda g: g["id"])


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-share-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир и темп — как в остальных проверках: наблюдателю нельзя отстать ни
    # на шаг, пропущенная дельта это пропущенный сбор.
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([server_binary, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        probe = connect()
        if probe is None:
            print("Сервер не поднялся")
            return 1
        probe.send({"type": "start_simulation"})

        goblins = None
        berries = []
        width = 0
        tick = 0
        # На какой клетке гоблин стоит, сколько тиков подряд и сдвинулся ли за
        # это время счёт ягод под ним.
        where = {}
        standing = collections.Counter()
        frozen = collections.Counter()
        worst = (0, None, None)

        while tick < ticks:
            message = probe.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                width = (message.get("area") or {}).get("width", 0)
                berries = list((message.get("layers") or {}).get("berries") or [])
                tick = message.get("tick", tick)
                continue
            if kind != "world_delta" or goblins is None:
                continue
            tick = message.get("tick", tick)
            # Слой правится до разбора шагов: и то, и другое случилось в одном
            # тике, а ягоды, которые гоблин видел, — те, что были к его началу.
            moved = set()
            pairs = message.get("berries") or []
            for i in range(0, len(pairs) - 1, 2):
                if pairs[i] < len(berries):
                    berries[pairs[i]] = pairs[i + 1]
                    moved.add(pairs[i])
            changes = message.get("goblins")
            if changes:
                apply_changes(goblins, changes)
            if not berries or not width:
                continue
            for goblin in goblins:
                cell = goblin["y"] * width + goblin["x"]
                if cell >= len(berries):
                    continue
                name = goblin["id"]
                if where.get(name) != cell:
                    where[name] = cell
                    standing[name] = 0
                    frozen[name] = 0
                    continue
                standing[name] += 1
                # Считается простой ТОЛЬКО на клетке, где есть что делить, и
                # только пока счёт не двигался: пустая клетка ни о чём не
                # говорит (там и брать нечего), а сдвинувшийся счёт — прямое
                # доказательство, что дележ работает.
                if berries[cell] > 0 and cell not in moved:
                    frozen[name] += 1
                    if frozen[name] > worst[0]:
                        worst = (frozen[name], name, cell)
                else:
                    frozen[name] = 0

        held, name, cell = worst
        if held >= STUCK:
            x, y = cell % width, cell // width
            print(f"ДЕЛЁЖ ВЫРОДИЛСЯ (тик {tick}): гоблин {name & 0xFFFF:04x} простоял {held} тиков "
                  f"на кусте {x},{y}, и счёт ягод под ним не сдвинулся ни разу.")
            print(f"  Столько может дать только доля, округлившаяся в ноль у ВСЕХ просящих "
                  f"(core/Share.hpp): куст стоит нетронутым, а голодные с него не уходят.")
            return 1
        print(f"Делимое расходится: тик {tick}, самая долгая стоянка на неубывающем кусте — "
              f"{held} тиков при пороге {STUCK}")
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

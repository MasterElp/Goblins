"""Каков на самом деле перепад поверхности воды в живом мире.

Пороги ряби (clients/desktop/src/WaterSprites.cpp: kStillDrop, kFastDrop)
выдумать нельзя. Клиент видит высоту и глубину сотыми долями прежней единицы
мира, а сколько это на настоящей карте — вопрос к карте, а не к глазу: на
пологой реке перепад между соседними клетками может оказаться и мельче
кванта, и тогда всякая вода будет считаться стоячей.

Меряется ровно то, что считает клиент (WaterSprites::flowAt): поверхность —
это высота дна плюс глубина, а перепад — самый крутой спуск среди восьми
соседей, поделённый на расстояние до соседа.

Сравнивать надо не одно число, а РАСПРЕДЕЛЕНИЕ: у половины водяных клеток
перепада нет вовсе (пруды и ровные плёсы), и среднее по всем сказало бы про
воду неправду.

Запуск (сервер поднимается сам, config.json пользователя не трогается):
    python3 tools/measure_water_flow.py
"""
import json
import math
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_probe import WebSocketProbe

from server_binary import find_server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 9131
# Соседи по кругу — те же восемь и в том же смысле, что в WaterSprites::flowAt.
D8 = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]
# Сотая доля: с такой точностью слои высоты и воды уходят по проводу
# (shared/protocol/WirePrecision.hpp), и мельче неё клиент не видит ничего.
FROM_HUNDREDTHS = 0.01
SETTLE_TICKS = 400


def wait_for(probe, kind, timeout=90):
    end = time.time() + timeout
    while time.time() < end:
        message = probe.recv(timeout=5)
        if message and message.get("type") == kind:
            return message
    return None


def steepest_drops(width, height, terrain, water):
    """Перепад поверхности на каждой водяной клетке — ровно как у клиента."""
    surface = [terrain[i] + water[i] for i in range(width * height)]
    drops = []
    for y in range(height):
        for x in range(width):
            i = y * width + x
            if water[i] <= 0:
                continue
            best = 0.0
            for dx, dy in D8:
                nx, ny = x + dx, y + dy
                if not (0 <= nx < width and 0 <= ny < height):
                    continue
                # Деление на расстояние: без него наискось перепад всегда
                # выходит круче просто потому, что клетка дальше.
                drop = (surface[i] - surface[ny * width + nx]) / math.hypot(dx, dy)
                best = max(best, drop)
            drops.append((best, water[i]))
    return drops


def main():
    binary = find_server(ROOT)
    if binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-water-flow-")
    # Настройки берутся у пользователя, но правятся в КОПИИ: config.json —
    # подобранный вручную мир, и трогать его проверке нечем.
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 200
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([binary, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        probe = None
        for _ in range(60):
            try:
                probe = WebSocketProbe(port=PORT)
                break
            except OSError:
                time.sleep(0.5)
        if probe is None:
            print("Сервер не поднялся")
            return 1

        init = wait_for(probe, "world_init")
        if init is None:
            print("Мир не приехал")
            return 1
        # Свежий сервер держит мир несозданным — просим создать его теми же
        # настройками, что и приехали, и ждём второго world_init.
        if init.get("generation"):
            probe.send({"type": "regenerate", "params": init["generation"]})
            init = wait_for(probe, "world_init") or init

        # Свежесозданный мир стоит на паузе (клиент снимает её кнопкой), а
        # мерить надо ТЕКУЩИЙ мир, а не тот, что вышел прямо из генератора.
        if init.get("paused"):
            probe.send({"type": "toggle_pause"})

        # Мир из генератора и мир после сотен тиков гидрологии — разные вещи:
        # русло за это время успевает промыться, а лужи на склонах — стечь.
        deadline = time.time() + 60
        while time.time() < deadline:
            message = probe.recv(timeout=5)
            if message and message.get("tick", 0) >= SETTLE_TICKS:
                # Полный снимок берётся отворачиванием и возвращением: дельта
                # везёт только изменившиеся клетки, а нужен весь слой целиком.
                probe.send({"type": "updates", "enabled": False})
                time.sleep(0.5)
                probe.send({"type": "updates", "enabled": True})
                init = wait_for(probe, "world_init", timeout=30) or init
                break

        width = init["area"]["width"]
        height = init["area"]["height"]
        layers = init["layers"]
        terrain = [v * FROM_HUNDREDTHS for v in layers["height"]]
        water = [v * FROM_HUNDREDTHS for v in layers["water"]]
        drops = steepest_drops(width, height, terrain, water)
        print("мир %dx%d, тик %d: водяных клеток %d из %d"
              % (width, height, init.get("tick", 0), len(drops), width * height))
        if not drops:
            print("Воды в мире нет — мерить нечего")
            return 0

        values = sorted(drop for drop, _ in drops)
        print("перепад поверхности на клетку, в единицах клиента:")
        for share in (0.25, 0.5, 0.75, 0.9, 0.95, 0.99):
            print("   %4.0f%%: %.4f" % (share * 100, values[int(len(values) * share)]))
        print("   больше некуда: %.4f" % values[-1])
        # Пороги клиента — тем же счётом, что и в WaterSprites.cpp.
        for name, cut in (("стоячей (kStillDrop)", 0.05), ("быстрее некуда (kFastDrop)", 0.5)):
            above = sum(1 for v in values if v >= cut)
            print("   выше порога %-26s %.2f: %5d клеток (%4.1f%%)"
                  % (name, cut, above, 100.0 * above / len(values)))
        depths = sorted(depth for _, depth in drops)
        print("глубина воды: медиана %.2f, больше некуда %.2f" % (depths[len(depths) // 2], depths[-1]))
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Сколько раз клиент зря пересобирает текстуру карты.

Текстура карты (clients/desktop/src/MapTexture.cpp) — это цвет каждой клетки
мира, посчитанный смешением полутора десятков слоёв. На мире 400x400 её
пересборка стоит около шести с половиной миллисекунд, и идёт она ВНУТРИ
кадра, на потоке отрисовки: при тридцати кадрах в секунду это пятая часть
всего бюджета кадра.

Пересобирается она тогда, когда изменился снимок мира. Вопрос, на который
отвечает эта мерка: а сколько сообщений сервера картинку карты вообще не
меняют — то есть сколько таких пересборок делается впустую.

Не меняют картинку:
  * pause_state, notice, world_list — в них нет ни одной клетки;
  * world_delta, в которой изменились только звери и гоблины (они рисуются
    поверх карты фигурами, а не текселем) либо не изменилось ничего вовсе
    (сервер шлёт снимки по своим часам, а не по тику мира).

Запуск (сервер поднимается сам, config.json пользователя не трогается):
    python3 tools/watch_map_rebuilds.py [секунд] [ширина] [высота]

Размер Области стоит задавать: доля изменившихся за дельту клеток — величина
относительная, и проверять её надо на том мире, о котором спрашивают.
"""
import collections
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_probe import WebSocketProbe

from server_binary import find_server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 9137

# Клеточные слои дельты — ровно те ключи, которые разбирает
# NetworkClient::applyChangedCells. Список повторён здесь, а не выведен из
# кода: мерка снаружи клиента и знает о нём только протокол.
CELL_KEYS = ("moisture", "trampled", "height", "water", "growth", "minerals", "humus",
             "carcass", "species", "seeds", "trees", "bushes", "berries", "store",
             "canopy", "bedding", "site", "site_material")


def touches_cells(message):
    """Изменила ли эта дельта хоть одну клетку."""
    for key in CELL_KEYS:
        value = message.get(key)
        if isinstance(value, list) and value:
            return True
    return False


def changed_cells(message):
    """Сколько записей о клетках в дельте и сколько среди них РАЗНЫХ клеток.

    Разница между этими двумя числами и есть цена, которую пришлось бы платить
    за складывание списка изменившихся клеток: слой в дельте свой у каждого, а
    клетка у них общая, и одна и та же клетка попадает в список столько раз, в
    скольких слоях она изменилась.
    """
    entries = 0
    unique = set()
    for key in CELL_KEYS:
        value = message.get(key)
        if not isinstance(value, list):
            continue
        for p in range(0, len(value) - 1, 2):
            entries += 1
            unique.add(value[p])
    return entries, len(unique)


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 60
    area = None
    if len(sys.argv) > 3:
        area = {"width": int(sys.argv[2]), "height": int(sys.argv[3])}
    binary = find_server(ROOT)
    if binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-map-rebuilds-")
    # Настройки берутся у пользователя, но правятся в КОПИИ: config.json —
    # подобранный вручную мир, и трогать его мерке нечем.
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    if area is not None:
        config["area"] = area
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

        init = None
        deadline = time.time() + 90
        while time.time() < deadline:
            message = probe.recv(timeout=5)
            if message and message.get("type") == "world_init":
                init = message
                break
        if init is None:
            print("Мир не приехал")
            return 1
        # Свежий сервер держит мир несозданным и на паузе — просим создать его
        # теми же настройками, что и приехали, и пустить время.
        if init.get("generation"):
            probe.send({"type": "regenerate", "params": init["generation"]})
            while time.time() < deadline:
                message = probe.recv(timeout=5)
                if message and message.get("type") == "world_init":
                    init = message
                    break
        if init.get("paused"):
            probe.send({"type": "toggle_pause"})

        cells = init.get("area", {})
        width = cells.get("width", 0)
        height = cells.get("height", 0)
        print("мир %dx%d, слушаем %d с" % (width, height, seconds))

        kinds = collections.Counter()
        idle_deltas = 0
        entries_total = 0
        unique_total = 0
        biggest = 0
        end = time.time() + seconds
        # Пауза дважды за прогон: так в счёт попадает и pause_state — то самое
        # сообщение, которое не меняет ни клетки, а карту пересобирает.
        toggles = [end - seconds * 0.6, end - seconds * 0.3]
        while time.time() < end:
            if toggles and time.time() > toggles[0]:
                probe.send({"type": "toggle_pause"})
                toggles.pop(0)
            message = probe.recv(timeout=5)
            if message is None:
                continue
            kind = message.get("type", "?")
            kinds[kind] += 1
            if kind == "world_delta":
                entries, unique = changed_cells(message)
                entries_total += entries
                unique_total += unique
                biggest = max(biggest, unique)
                if unique == 0:
                    idle_deltas += 1

        total = sum(kinds.values())
        if total == 0:
            print("Сервер молчал — мерить нечего")
            return 1
        # Пересборка сегодня идёт на КАЖДОЕ сообщение: версию снимка поднимает
        # NetworkClient::publishState, а её и сверяет MapTexture::Cache.
        wasted = total - (kinds["world_delta"] - idle_deltas) - kinds["world_init"]
        print("сообщений всего: %d" % total)
        for kind, count in kinds.most_common():
            print("   %-14s %5d" % (kind, count))
        print("дельт без единой изменившейся клетки: %d из %d"
              % (idle_deltas, kinds["world_delta"]))
        print("пересборок карты впустую: %d из %d (%.0f%%)"
              % (wasted, total, 100.0 * wasted / total))
        живых = kinds["world_delta"] - idle_deltas
        if живых:
            cells = width * height
            print("в дельте с клетками: записей %.0f, разных клеток %.0f (в среднем), "
                  "больше некуда %d"
                  % (entries_total / живых, unique_total / живых, biggest))
            print("это %.2f%% мира; полная пересборка считает все %d клеток"
                  % (100.0 * (unique_total / живых) / cells, cells))
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())

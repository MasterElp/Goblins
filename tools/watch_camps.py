"""Возник ли лагерь — числом, а не на глаз.

Вопрос, ради которого затевались тропы и годность утоптанного для отдыха:
сходятся ли дороги гоблинов в ОДНО место и спит ли кто-нибудь в этом месте.
Глазами карта троп выглядит убедительно всегда — паутина линий кажется
осмысленной, даже когда это просто следы блуждания.

Меры две, и порознь они лгут обе:

  * **пятна утоптанного** — связные области земли выше порога. Одно большое
    пятно вместо десятка мелких означает, что дороги сошлись;
  * **спящие внутри пятен** — доля гоблинов с желанием `rest`, оказавшихся
    в самом крупном пятне и в пятнах вообще.

Вытоптанный перекрёсток, на котором никто не спит, — это дорога, а не
лагерь. Пятно, в котором спят, но которое одно из сорока таких же, — это
ночёвка, а не место притяжения. Лагерь — когда оба числа велики разом.

С появлением ноши (core/Carry.hpp) добавилась третья мера, и она про то же
самое с другой стороны: **где лежат кучи**. Еда, принесённая руками, должна
оседать У МЕСТА ОТДЫХА, а не там, где её нашли, — значит, доля еды, лежащей
внутри пятен, обязана быть заметно выше доли самих пятен на карте. Кучи,
рассыпанные ровно, означают, что дом в памяти не работает как адрес.

С появлением построек (core/Build.hpp) добавилась мера, ради которой шаг и
делался: **средняя годность тех клеток, на которых спят**. Она отвечает на
вопрос, который до сих пор задать было нельзя, — стал ли мир лучше от того,
что в нём живут. Вырасти она должна не оттого, что гоблины нашли места
получше, а оттого, что сделали получше те, где уже спали.

Рядом печатается, где стоят постройки: внутри пятен утоптанного или где
попало. Навес посреди чистого поля означает, что замысел ставится не там, где
живут.

Четвёртая мера, справочная: **доля суши, годной для отдыха**. Она отвечает не
про лагерь, а про порог kRestGood — сколько мест мир вообще предлагает. Её
формула здесь переписана с core/Rest.hpp, а вот ЧИСЛА берутся у сервера
(он шлёт свои константы в world_init), чтобы переписанное не разъехалось с
законом молча при первой же подкрутке.

Числа сами по себе не значат ничего, значит СРАВНЕНИЕ: тот же seed, то же
число тиков, выключатель троп off и on. Поэтому запускать её надо парой.

Запуск:
    python3 tools/watch_camps.py [тиков] [on|off]
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
PORT = 9110
TICKS = 6000
# С какой утоптанности клетка считается частью пятна, в сотых (слой приходит
# сотыми, shared/protocol/WirePrecision.hpp). Выше порога троп в
# watch_goblin_routes.py: там вопрос "ходили ли здесь", здесь — "обжито ли
# это место", а обжитое место топчут не мимоходом.
CAMP = 25
# Меньше этого пятно не лагерь, а просто набитый угол.
MIN_CAMP_CELLS = 4


def clusters(trampled, width, height):
    """Связные пятна утоптанного (восемь соседей) — списком размеров и центров."""
    seen = [False] * len(trampled)
    found = []
    for start in range(len(trampled)):
        if seen[start] or trampled[start] < CAMP:
            continue
        stack = [start]
        seen[start] = True
        cells = []
        while stack:
            cell = stack.pop()
            cells.append(cell)
            x, y = cell % width, cell // width
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if not (0 <= nx < width and 0 <= ny < height):
                        continue
                    j = ny * width + nx
                    if seen[j] or trampled[j] < CAMP:
                        continue
                    seen[j] = True
                    stack.append(j)
        if len(cells) >= MIN_CAMP_CELLS:
            cx = sum(c % width for c in cells) // len(cells)
            cy = sum(c // width for c in cells) // len(cells)
            found.append((len(cells), cx, cy, set(cells)))
    found.sort(key=lambda item: item[0], reverse=True)
    return found


def rest_quality(layers, constants, i):
    """Годность одной клетки по закону core/Rest.hpp.

    Формула переписана, ЧИСЛА взяты у сервера (он шлёт константы в
    world_init): подкрутят закон в ядре — изменится и это, без правок здесь.
    Слои приходят сотыми, а закон живёт в тысячных (core/Scale.hpp), отсюда
    множители: сравнивать сотые с порогом в тысячных значило бы ошибиться
    ровно в десять раз, и однажды на этом уже обожглись.
    """
    def layer(name):
        values = layers.get(name) or []
        return values[i] if i < len(values) else 0

    quality = constants["kRestBase"]
    quality += (100 - min(100, layer("moisture"))) * constants["kRestDryWeight"] // 100
    # Крыша одна: лучшее из кроны и навеса, а не сумма.
    tree_roof = constants["kRestShelter"] if layer("trees") >= 0 else 0
    canopy_roof = min(100, layer("canopy")) * constants.get("kRestCanopy", 0) // 100
    quality += max(tree_roof, canopy_roof)
    quality += min(100, layer("bedding")) * constants.get("kRestBedding", 0) // 100
    quality -= min(100, layer("rockiness")) * constants["kRestRockPenalty"] // 100
    quality += min(100, layer("trampled")) * constants["kRestTrodden"] // 100
    return quality


def rest_share(layers, constants, width, height):
    """Какая доля суши годится, чтобы лечь, — по закону core/Rest.hpp.

    Формула переписана, числа взяты у сервера: подкрутят порог в ядре —
    изменится и это, без правок здесь. Совсем без переписывания не обойтись:
    закон живёт в C++, а сюда приезжают только слои.
    """
    need = ("kRestBase", "kRestDryWeight", "kRestShelter", "kRestRockPenalty", "kRestTrodden", "kRestGood")
    if any(name not in constants for name in need):
        return None
    moisture = layers.get("moisture") or []
    rockiness = layers.get("rockiness") or []
    trampled = layers.get("trampled") or []
    trees = layers.get("trees") or []
    water = layers.get("water") or []
    if not moisture or not rockiness:
        return None
    good = 0
    land = 0
    for i in range(len(moisture)):
        if i < len(water) and water[i] > 0:
            continue  # вода — не суша, лечь туда нельзя
        land += 1
        if rest_quality(layers, constants, i) >= constants["kRestGood"]:
            good += 1
    return (good * 100.0 / land) if land else None


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    trampling = (sys.argv[2].lower() != "off") if len(sys.argv) > 2 else True
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-camps-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир и темп — как в watch_goblin_routes.py и по той же причине:
    # наблюдателю нельзя отстать, иначе пропущенные дельты это пропущенные
    # шаги и незамеченные тропы.
    config["area"] = {"width": 96, "height": 96}
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
    config.setdefault("terrain", {}).setdefault("toggles", {})["trampling"] = trampling
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([server_binary, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        probe = None
        for _ in range(60):
            try:
                probe = WebSocketProbe(port=PORT)
                break
            except OSError:
                time.sleep(1)
        if probe is None:
            print("Сервер не поднялся")
            return 1

        probe.send({"type": "start_simulation"})
        goblins = None
        tick = 0
        width = height = 0
        layers = {}
        constants = {}
        while tick < ticks:
            message = probe.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                tick = message.get("tick", tick)
                area = message.get("area") or {}
                width, height = area.get("width", 0), area.get("height", 0)
                layers = {name: list(values) for name, values in (message.get("layers") or {}).items()}
                constants = {item["name"]: int(item["value"]) for item in (message.get("constants") or [])}
                continue
            if kind != "world_delta":
                continue
            tick = message.get("tick", tick)
            # Слои правятся дельтами — теми же парами "индекс, значение", по
            # которым их держит клиент.
            for name in ("trampled", "trees", "water", "store", "canopy", "bedding", "site"):
                pairs = message.get(name) or []
                target = layers.setdefault(name, [])
                for i in range(0, len(pairs) - 1, 2):
                    if pairs[i] < len(target):
                        target[pairs[i]] = pairs[i + 1]
            changes = message.get("goblins")
            if goblins is None or not changes:
                continue
            # Порядок применения — тот же, что у клиента: правки по индексам
            # в ПРЕЖНЕМ списке, потом ушедшие, потом родившиеся.
            triples = changes.get("pos", [])
            for i in range(0, len(triples) - 2, 3):
                if triples[i] < len(goblins):
                    goblins[triples[i]]["x"], goblins[triples[i]]["y"] = triples[i + 1], triples[i + 2]
            desires = changes.get("desire", [])
            for i in range(0, len(desires) - 1, 2):
                if desires[i] < len(goblins):
                    goblins[desires[i]]["desire"] = desires[i + 1]
            gone = set(changes.get("gone", []))
            if gone:
                goblins = [g for i, g in enumerate(goblins) if i not in gone]
            born = changes.get("born", [])
            if born:
                goblins = sorted(goblins + list(born), key=lambda g: g["id"])

        trampled = layers.get("trampled") or []
        if not trampled or not width:
            print("Слой троп не приехал — мерить нечего")
            return 1

        found = clusters(trampled, width, height)
        alive = goblins or []
        resting = [g for g in alive if g.get("desire") == "rest"]
        inside = set()
        for _, _, _, cells in found:
            inside |= cells
        biggest = found[0] if found else None

        def within(group, cells):
            return sum(1 for g in group if (g["y"] * width + g["x"]) in cells)

        print(f"Тик {tick}, гоблинов {len(alive)}, из них лежит {len(resting)}")
        if not found:
            print("ЛАГЕРЯ НЕТ: ни одного пятна утоптанного крупнее "
                  f"{MIN_CAMP_CELLS} клеток")
        else:
            print(f"ПЯТНА: {len(found)}, крупнейшее {biggest[0]} клеток "
                  f"вокруг ({biggest[1]},{biggest[2]}), всего в пятнах {len(inside)} клеток")
            print(f"  в пятнах гоблинов {within(alive, inside)} из {len(alive)}, "
                  f"из них лежащих {within(resting, inside)} из {len(resting)}")
            print(f"  в крупнейшем: гоблинов {within(alive, biggest[3])}, "
                  f"лежащих {within(resting, biggest[3])}")
            print("  пятна по убыванию: " +
                  ", ".join(f"({x},{y}):{size}" for size, x, y, _ in found[:5]))

        store = layers.get("store") or []
        if store:
            heaps = [(i, value) for i, value in enumerate(store) if value > 0]
            food_total = sum(value for _, value in heaps)
            food_inside = sum(value for i, value in heaps if i in inside)
            biggest_food = sum(value for i, value in heaps if biggest and i in biggest[3])
            land = sum(1 for i, value in enumerate(layers.get("moisture") or [])
                       if i >= len(layers.get("water") or []) or (layers["water"][i] == 0))
            spots = len(inside) * 100.0 / land if land else 0.0
            if not heaps:
                print("КУЧ НЕТ: ничего никуда не принесли")
            else:
                print(f"КУЧИ: {len(heaps)} клеток с запасом, еды в них {food_total}")
                print(f"  внутри пятен {food_inside * 100.0 / food_total:.1f}% этой еды "
                      f"(сами пятна — {spots:.1f}% суши), в крупнейшем пятне {biggest_food}")

        canopy = layers.get("canopy") or []
        bedding = layers.get("bedding") or []
        sites = layers.get("site") or []
        built = [i for i, v in enumerate(canopy) if v > 0] + [i for i, v in enumerate(bedding) if v > 0]
        planned = [i for i, v in enumerate(sites) if v > 0]
        if not built and not planned:
            print("ПОСТРОЕК НЕТ: ни одной клетки с навесом, подстилкой или замыслом")
        else:
            inside_built = sum(1 for i in set(built) if i in inside)
            print(f"ПОСТРОЙКИ: навесов {sum(1 for v in canopy if v > 0)}, "
                  f"подстилок {sum(1 for v in bedding if v > 0)}, "
                  f"строек начато {len(planned)}")
            if built:
                print(f"  внутри пятен {inside_built} из {len(set(built))}; "
                      f"средняя прочность навеса {sum(canopy) / max(1, sum(1 for v in canopy if v > 0)):.0f} "
                      f"из 100")

        # Главное число шага: стало ли лучше там, где спят.
        if constants and resting:
            qualities = [rest_quality(layers, constants, g["y"] * width + g["x"]) for g in resting]
            print(f"ГОДНОСТЬ МЕСТА СНА: в среднем {sum(qualities) / len(qualities):.0f} "
                  f"(порог {constants.get('kRestGood')}), считано по {len(qualities)} лежащим")

        share = rest_share(layers, constants, width, height)
        if share is not None:
            print(f"  справка — годной для отдыха суши: {share:.1f}% "
                  f"(порог kRestGood = {constants.get('kRestGood')})")
        return 0
    finally:
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

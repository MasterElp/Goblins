"""Сверка двух ядер на одном seed: что изменилось в мире от правки закона.

Зачем. Проверка "все значения целые и в объявленных пределах" проходит
полностью на коде, где величина в тысячу раз не та (см. CLAUDE.md, раздел
про величины). Единственный способ увидеть такое — прогнать один и тот же
мир двумя ядрами и сличить не типы, а числа.

Ожидание от сверки зависит от того, что менялось, и разница здесь несущая:

  * Вынос закона, переименование, перестановка кода — ни одно число не
    меняется, и сверка обязана дать ТОЧНОЕ совпадение, включая поимённый
    список существ с координатами. Расхождение в единицу означает, что при
    переносе что-то потерялось.

  * Изменение самого закона мира — совпадения не будет и не должно быть.
    Смотреть надо на ПОРЯДОК величин: поголовье, объём воды и минералы
    обязаны остаться теми же по масштабу. Расхождение в разы — повод
    разбираться, а не строчка в отчёте.

Как получить второе ядро. Скрипт ничего не собирает сам — он принимает два
готовых бинарника, и это осознанно: собрать "как было" можно по-разному
(HEAD, тег, другая ветка), и выбор тут за тем, кто сверяет.

    cp build/server/Release/server.exe /tmp/after.exe
    git stash                       # или: git show <коммит>:<файл> > <файл>
    ./build.sh && cp build/server/Release/server.exe /tmp/before.exe
    git stash pop
    python3 tools/compare_cores.py /tmp/before.exe /tmp/after.exe

Сличать можно только на ОДНОМ И ТОМ ЖЕ тике, иначе разойдётся не закон, а
время. Пауза приходит по сети и мир успевает шагнуть ещё раз, поэтому тик
держится нарочно медленным (пауза срабатывает заметно быстрее, чем проходит
тик), а если прогоны всё же встали на разных тиках — отставший догоняется.

Запуск:
    python3 tools/compare_cores.py <before> <after> [тиков] [seed]
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_BEFORE = 9331
PORT_AFTER = 9332
TICKS = 4000
# Медленный тик — не замедление ради замедления: пауза идёт по сети и
# ставится сетевым потоком, и чем длиннее тик, тем надёжнее мир встаёт ровно
# там, где просили. При тике в миллисекунду он проскакивал цель на десятки.
TICK_MS = 20
# Сколько раз догонять отставший прогон, прежде чем сдаться.
ALIGN_TRIES = 4
# Ниже этого прогон бессмыслен: пока сервер не упрётся в свой предел
# неразгребённого (см. snapshot_interval_ms ниже), наблюдатель отстаёт, и
# остановка приходит не туда. На цели в сотню тиков это давало промах в
# двадцать раз.
MIN_TICKS = 500


def next_tick(probe, tick):
    """Следующее сообщение о состоянии мира — и номер тика из него."""
    silent = 0
    while True:
        message = probe.recv(timeout=60)
        if message is None:
            silent += 1
            if silent > 5:
                raise SystemExit(f"Мир встал на тике {tick}")
            continue
        if message.get("type") in ("world_delta", "world_init"):
            return message.get("tick", tick)


def run(server_exe, port, target, seed):
    """Прогнать мир до тика target и снять с него все величины."""
    workdir = tempfile.mkdtemp(prefix="goblins-compare-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = port
    config["tick_interval_ms"] = TICK_MS
    # Снимки — как можно чаще, и это НЕ расточительство, а единственное, что
    # удерживает наблюдателя в настоящем.
    #
    # Наблюдатель разбирает снимок примерно за то же время, за какое сервер
    # его шлёт, поэтому любое набежавшее отставание держится навсегда, а мир
    # уходит вперёд: остановка по отставшему тику приходит туда, где мира уже
    # нет (однажды — цель 600 и остановка на 2350). Само это не рассасывается.
    #
    # Спасает встречный механизм сервера: клиенту, у которого набралось
    # больше мегабайта неразгребённого, он перестаёт слать вовсе
    # (clientsAreBehind в NetworkServer.cpp). Частые снимки этот предел
    # пробивают сразу, и дальше сервер шлёт ровно с той скоростью, с какой
    # наблюдатель успевает читать. Отставание перестаёт копиться, и пауза
    # попадает туда, куда просили.
    #
    # Редкие снимки этот тормоз не включают — и всё ломается. Проверено:
    # snapshot_interval_ms = 20 давал промах в полторы тысячи тиков.
    config["snapshot_interval_ms"] = 1
    config["seed"] = seed
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([server_exe, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        probe = None
        for _ in range(60):
            try:
                probe = WebSocketProbe(port=port)
                break
            except OSError:
                time.sleep(1)
        if probe is None:
            raise SystemExit(f"Сервер {server_exe} не поднялся")

        probe.send({"type": "start_simulation"})
        tick = 0
        while tick < target:
            tick = next_tick(probe, tick)

        # Пауза ставится ровно здесь, а не после разбора: между "увидел
        # нужный тик" и "мир встал" проходит один обмен по сети, и всё, что
        # делается в этом промежутке, мир доживает лишними тиками.
        probe.send({"type": "toggle_pause"})

        deadline = time.time() + 60
        while time.time() < deadline:
            message = probe.recv(timeout=30)
            if message and message.get("type") == "pause_state":
                break

        # Полный мир выпрашиваем вторым подключением: на нового наблюдателя
        # сервер рассылает world_init всем. Тот же приём, что в
        # check_animal_delta.py, и по той же причине — отвернуться и
        # посмотреть снова нельзя, отвернувшемуся нарочно не шлют ничего.
        watcher = WebSocketProbe(port=port)
        full = None
        deadline = time.time() + 60
        while time.time() < deadline:
            message = probe.recv(timeout=30)
            if message and message.get("type") == "world_init":
                full = message
                break
        watcher.s.close()
        probe.s.close()
        if full is None:
            raise SystemExit("Сервер не прислал world_init")
        return summarize(full)
    finally:
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(workdir, ignore_errors=True)


def summarize(message):
    """Все величины мира числами — по тому, что реально приехало.

    Список слоёв и список существ не зашиты: слои берутся такими, какие
    сервер прислал, а существами считается всякий список карточек с
    полем id. Появится в протоколе новый слой или новое племя — сверка
    увидит их сама, и не придётся вспоминать, что сюда надо дописать.
    Ровно эта забывчивость уже дважды стоила проекту потерянных настроек.
    """
    totals = {"тик": message.get("tick")}
    named = {}

    for name, values in sorted((message.get("layers") or {}).items()):
        if not isinstance(values, list) or not values:
            continue
        # Сумма — всегда: как отпечаток слоя она годится любому, даже там,
        # где сама величина бессмысленна (сумма номеров видов ничего не
        # значит, но расхождение в ней означает разошедшийся мир).
        totals[f"сумма: {name}"] = sum(values)
        # А вот штуки — только там, где есть "здесь никого" (-1): у слоя
        # видов только счёт и читается глазами. Различать слои по имени тут
        # нельзя — новый слой появится, а список не поправят.
        if any(v < 0 for v in values):
            totals[f"клеток: {name}"] = sum(1 for v in values if v >= 0)

    for key, value in sorted(message.items()):
        if not isinstance(value, list) or not value:
            continue
        if not all(isinstance(item, dict) and "id" in item for item in value):
            continue
        totals[f"{key}: всего"] = len(value)
        for field in ("kind", "tribe"):
            kinds = sorted({item[field] for item in value if field in item})
            for kind in kinds:
                totals[f"{key}: {kind}"] = sum(1 for item in value if item.get(field) == kind)
        for field in ("growth", "health"):
            if any(field in item for item in value):
                totals[f"{key}: сумма {field}"] = sum(item.get(field, 0) for item in value)
        named[key] = sorted(json.dumps(item, sort_keys=True, ensure_ascii=False) for item in value)

    return totals, named


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1
    before_exe, after_exe = args[0], args[1]
    ticks = int(args[2]) if len(args) > 2 else TICKS
    if ticks < MIN_TICKS:
        print(f"Цель {ticks} слишком мала: первые сотни тиков уходят на то, "
              f"чтобы наблюдатель разгрёб поток. Не меньше {MIN_TICKS}.")
        return 1
    seed = int(args[3]) if len(args) > 3 else \
        json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))["seed"]

    for path in (before_exe, after_exe):
        if not os.path.exists(path):
            print(f"Нет бинарника: {path}")
            return 1

    before = run(before_exe, PORT_BEFORE, ticks, seed)
    after = run(after_exe, PORT_AFTER, ticks, seed)
    for _ in range(ALIGN_TRIES):
        if before[0]["тик"] == after[0]["тик"]:
            break
        goal = max(before[0]["тик"], after[0]["тик"])
        print(f"  тики разошлись ({before[0]['тик']} и {after[0]['тик']}) — догоняю до {goal}")
        if before[0]["тик"] < goal:
            before = run(before_exe, PORT_BEFORE, goal, seed)
        else:
            after = run(after_exe, PORT_AFTER, goal, seed)

    if before[0]["тик"] != after[0]["тик"]:
        print(f"Не удалось свести прогоны на один тик: {before[0]['тик']} и {after[0]['тик']}")
        return 1

    return report(before, after, seed)


def report(before, after, seed):
    totals_before, named_before = before
    totals_after, named_after = after

    print(f"\nТик {totals_before['тик']}, seed {seed}\n")
    width = max(len(k) for k in totals_before)
    differing = 0
    for key in totals_before:
        old = totals_before[key]
        new = totals_after.get(key)
        same = old == new
        differing += 0 if same else 1
        mark = "" if same else "   <-- расхождение"
        print(f"{key:<{width}} {old:>14} {new:>14}{mark}")

    lists_differ = []
    for key in sorted(set(named_before) | set(named_after)):
        mine = named_before.get(key, [])
        theirs = named_after.get(key, [])
        if mine == theirs:
            print(f"\nпоимённо ({key}, {len(mine)} записей): совпадает")
            continue
        lists_differ.append(key)
        print(f"\nпоимённо ({key}): РАСХОДИТСЯ — до {len(mine)}, после {len(theirs)}")
        for old, new in [(o, n) for o, n in zip(mine, theirs) if o != n][:3]:
            print(f"   до:    {old}")
            print(f"   после: {new}")

    if differing or lists_differ:
        print(f"\nМиры разошлись: {differing} величин, списки {lists_differ or 'сошлись'}.")
        print("Если правился ЗАКОН — смотри на порядок величин, расхождение в разы"
              " значит ошибку.\nЕсли код только ПЕРЕКЛАДЫВАЛСЯ — расхождение в единицу"
              " уже означает потерю.")
        return 1

    print("\nМиры совпадают полностью — до последнего числа и до каждой карточки.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Работает ли нрав — числом, а не на глаз.

Нрав и связи ловятся глазами хуже всего в этом мире. Гоблин, который никогда
не заговаривает первым, выглядит точно так же, как тот, кто заговаривает
постоянно: оба ходят по своим делам. А знакомство и вовсе невидимо — оно
живёт числом в чужой голове.

Поэтому здесь четыре вопроса, и каждый задан так, чтобы на него можно было
ответить "нет".

1. **Общительность что-нибудь делает?** Гоблины делятся пополам по нраву, и
   у болтливой половины доля тиков в желании "talk" обязана быть заметно
   выше. Не выше — значит число в компоненте лежит, а мир его не читает.

2. **Постоянство что-нибудь делает?** Та же половинная проба, но мера
   другая: какую долю всего своего тепла гоблин держит в ОДНОМ, самом
   близком знакомстве. У постоянного связь должна быть глубокой и одной, у
   непостоянного — россыпью тёплых понемногу.

3. **Знание расходится по связям?** Сколько гоблинов помнит самое
   популярное место мира. Это единственное число, которое нельзя прочесть
   из одного прогона: врозь оно не значит ничего, значит только разница
   между `talk_urge` 0 и 20.

4. **Связи вообще заводятся?** Сколько знакомых у гоблина в среднем и
   насколько тёплое у него самое близкое знакомство. При `talk_urge` 0 оба
   обязаны быть нулями — если нет, тепло берётся откуда-то ещё, и это ошибка.

Чего здесь НЕТ: счёта самих разговоров и дележа ношей. Ни то, ни другое не
уезжает на провод (разговор — событие тика, а не состояние мира), и считать
их пришлось бы по косвенным признакам, то есть гадать. Тоска (`talking`) и
связи (`faces`) при этом видны честно, через панель наблюдения, и по ним
всё и меряется.

Замер — это ПАРА прогонов одного бинарника, как и у троп:

    python3 tools/watch_goblin_talk.py 6000 0
    python3 tools/watch_goblin_talk.py 6000 20

Двумя разными сборками мерить нельзя — сравнивалось бы заодно всё, что
успело измениться в ядре.
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
PORT = 9111
TICKS = 6000
# Сколько гоблинов опрашивается панелью наблюдения в конце. Опрос идёт по
# одному и с двумя пересылками на каждого, поэтому на большом поголовье он
# длиннее самого прогона. Полторы сотни — заведомо больше, чем живёт на карте
# 96x96 (02_CorePrinciples.md, п.16: десятки), то есть обычно опрашиваются все.
WATCH_LIMIT = 150


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def apply_changes(goblins, changes):
    """Тот же разбор дельты, каким её применяет клиент (applyCreatureChanges).

    Списан оттуда же, откуда и в check_animal_delta.py, и по той же причине:
    номера в дельте — это места в списке, а места сдвигаются рождениями и
    смертями. Считать желания, не поддерживая список, значит считать чужие.
    """
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


def watch(listen, control, goblin_id, seconds=20):
    """Подробности одного гоблина: нрав, склонности, память и знакомые.

    Прежнюю цель приходится сперва отпускать: сервер шлёт "watched", только
    когда выбор ИЗМЕНИЛСЯ, и повторная просьба следить за тем же самым не
    даст ответа никогда (см. check_world_save.py).
    """
    control.send({"type": "watch", "kind": "none", "id": 0, "x": 0, "y": 0})
    while listen.recv(0.2) is not None:
        pass
    control.send({"type": "watch", "kind": "goblin", "id": goblin_id, "x": 0, "y": 0})
    deadline = time.time() + seconds
    while time.time() < deadline:
        message = listen.recv(min(5.0, max(0.1, deadline - time.time())))
        if message is None:
            continue
        watched = message.get("watched")
        if isinstance(watched, dict) and watched.get("id") == goblin_id:
            return watched
    return None


def group_of(watched, title):
    for group in (watched or {}).get("groups") or []:
        if group.get("title") == title:
            return {name: value for name, value in group.get("values") or []}
    return {}


def halves(rows, by, measure):
    """Проба половинами: среднее measure у верхней и нижней половины по by.

    Половинами, а не коэффициентом связи: коэффициент пришлось бы объяснять,
    а "у болтливой половины втрое больше" читается сразу и врёт ровно
    настолько же. Меньше четырёх наблюдений — не проба, а совпадение.
    """
    usable = [r for r in rows if by in r and measure in r]
    if len(usable) < 4:
        return None
    usable.sort(key=lambda r: r[by])
    edge = len(usable) // 2
    low = usable[:edge]
    high = usable[len(usable) - edge:]
    return (sum(r[measure] for r in low) / len(low), sum(r[measure] for r in high) / len(high))


def report(name, split, units=""):
    if split is None:
        print(f"  {name}: гоблинов слишком мало, чтобы делить пополам")
        return
    low, high = split
    verdict = "работает" if high > low * 1.2 else "НЕ ВИДНО РАЗНИЦЫ"
    print(f"  {name}: нижняя половина {low:.2f}{units}, верхняя {high:.2f}{units} — {verdict}")


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    urge = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    # Разум (core/Mind.hpp): о чём заговорить — его решение, а не закон мира.
    # Жадный всегда берёт самую сильную склонность, жребий говорит по всем.
    # Разница видна в том, ЧТО расходится слухом: одни ли места одного рода
    # или разные.
    mind = sys.argv[3] if len(sys.argv) > 3 else "greedy"

    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-talk-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир и темп — как в остальных проверках: сервер не читает команд с
    # подключения, которое сам заливает (см. check_animal_delta.py).
    config["area"] = {"width": 96, "height": 96}
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
    # Тоска включается и выключается здесь, а не пересборкой: замер обязан
    # сравнивать один и тот же бинарник с собой.
    config.setdefault("goblins", {})["talk_urge"] = urge
    config.setdefault("terrain", {}).setdefault("toggles", {})["lottery_mind"] = mind == "lottery"
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
        tick = 0
        # Сколько тиков каждый провёл в желании поговорить и сколько всего
        # прожил на глазах у наблюдателя. Второе нужно затем, что рождённые
        # посреди прогона жили меньше и по голому счёту выглядели бы
        # молчунами, кем бы они ни были.
        talking = collections.Counter()
        watched_ticks = collections.Counter()
        while tick < ticks:
            message = listen.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                tick = message.get("tick", tick)
                continue
            if kind != "world_delta" or goblins is None:
                continue
            tick = message.get("tick", tick)
            changes = message.get("goblins")
            if changes:
                apply_changes(goblins, changes)
            for goblin in goblins:
                watched_ticks[goblin["id"]] += 1
                if goblin.get("desire") == "talk":
                    talking[goblin["id"]] += 1

        control.send({"type": "stop_simulation"})
        time.sleep(0.5)
        while listen.recv(0.5) is not None:
            pass

        # Нрав, склонности и знакомых можно взять только поимённо: в общем
        # списке их нет и не должно быть (см. buildWatchedJson).
        rows = []
        knowers = collections.Counter()
        for goblin in goblins[:WATCH_LIMIT]:
            seen = watch(listen, control, goblin["id"])
            if seen is None:
                continue
            nature = group_of(seen, "Character")
            faces = seen.get("faces") or []
            warmth = sorted((face.get("warmth", 0) for face in faces), reverse=True)
            lived = max(1, watched_ticks[goblin["id"]])
            row = {
                "sociable": nature.get("sociable", 0),
                "loyal": nature.get("loyal", 0),
                "diligent": nature.get("diligent", 0),
                # Доля прожитых тиков, а не их число: рождённый посреди
                # прогона иначе всегда выглядел бы молчаливее старожила.
                "talk_share": 100.0 * talking[goblin["id"]] / lived,
                "faces": len(faces),
                "closest": warmth[0] if warmth else 0,
                # Какую долю всего тепла держит одно самое близкое
                # знакомство. Сто — весь круг сошёлся на одном; двадцать —
                # пятеро приятелей поровну.
                "focus": (100.0 * warmth[0] / sum(warmth)) if warmth else 0.0,
            }
            rows.append(row)
            for place in seen.get("knows") or []:
                knowers[(place.get("x"), place.get("y"), place.get("kind"))] += 1

        if not rows:
            print("Ни один гоблин не ответил — мерить нечего")
            return 1

        alive = len(rows)
        avg_faces = sum(r["faces"] for r in rows) / alive
        avg_closest = sum(r["closest"] for r in rows) / alive
        avg_talk = sum(r["talk_share"] for r in rows) / alive
        popular = knowers.most_common(1)[0] if knowers else None

        # Сколько РАЗНЫХ родов мест разошлось по головам: при жадном разуме
        # гоблин всегда говорит об одном и том же, и слухом расходится один
        # род, а не все.
        toldKinds = collections.Counter(place[2] for place in knowers)
        print(f"talk_urge = {urge}, разум {mind}, тиков {tick}, опрошено гоблинов {alive}")
        print(f"  родов мест в чужих головах: {len(toldKinds)} ({dict(toldKinds)})")
        print(f"  в желании поговорить: {avg_talk:.1f}% прожитых тиков")
        print(f"  знакомых у гоблина: {avg_faces:.1f}, тепло самого близкого: {avg_closest:.0f} из 100")
        if popular is None:
            print("  никто ничего не помнит — расхождению знания взяться неоткуда")
        else:
            place, count = popular
            print(f"  самое известное место {place[2]} в {place[0]},{place[1]}: "
                  f"его помнят {count} из {alive} ({100.0 * count / alive:.0f}%)")
        report("общительность -> доля тиков в разговоре", halves(rows, "sociable", "talk_share"), "%")
        report("постоянство -> тепла в одном знакомстве", halves(rows, "loyal", "focus"), "%")
        print("Сравнивать эти числа надо с прогоном при другом talk_urge — врозь они не значат ничего.")
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

"""Повторяются ли маршруты гоблинов — числом, а не на глаз.

Вопрос, ради которого затевалась память места: ходит гоблин каждый раз
заново или возвращается туда, где уже был. Глазами это не различить — и то,
и другое выглядит как существо, идущее по своим делам.

Главная мера — **возвраты**: какая доля шагов приводит гоблина на клетку,
где он САМ уже был. Считается по каждому отдельно и потом усредняется.

Именно по каждому, и это не мелочь. Первой мерой была сосредоточенность
всего движения (доля клеток, дающая половину шагов), и она оказалась
негодной: её забивает не память, а скученность самого мира — вода лежит в
двух местах, и к ней сходятся все, помнят они дорогу или нет. Хуже того, она
зависит от численности: удачливое поголовье растёт, расходится шире и по
этой мере выглядит МЕНЕЕ собранным, чем вымирающее. Первый же замер это и
показал — с памятью 4.1% против 3.5% без неё, при вдвое большем поголовье.
Сосредоточенность считается по-прежнему, но как справка, а не как ответ.

Возвраты от численности не зависят вовсе: гоблин сравнивается сам с собой.

Числа сами по себе ничего не значат, значит только СРАВНЕНИЕ: тот же seed,
то же число тиков, сборка до и после. Поэтому запускать её надо парой.

Считается по дельтам, тем же способом, каким клиент держит список
(`goblins.pos`), — то есть по тем самым шагам, которые видит наблюдатель.

С появлением троп (core/Trample.hpp) сюда добавились три числа про саму
землю. Главное из них — **доля шагов по утоптанному**: она отвечает на
вопрос, держит ли тропа сама себя. Если по натоптанному ходят не чаще, чем
его вообще есть на карте, значит притяжение не работает и тропа — просто
след, а не дорога.

Замер троп — это ПАРА прогонов одного бинарника: `toggles.trampling`
выключен и включён. Двумя разными сборками мерить нельзя — сравнивалось бы
заодно всё, что успело измениться в ядре.

Запуск:
    python3 tools/watch_goblin_routes.py [тиков] [on|off]
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
PORT = 9109
TICKS = 6000
# С какой утоптанности клетка считается тропой, в сотых (слой приходит
# сотыми, shared/protocol/WirePrecision.hpp). Десятая доля — это два прохода
# взрослого: ниже неё след ещё не отличим от случайной прогулки.
TRAIL = 10


def concentration(visits):
    """Доля клеток, дающая половину посещений, в процентах.

    Пятьдесят — движение размазано совершенно ровно. Единицы — почти всё
    хождение приходится на несколько клеток, то есть на тропы.
    """
    if not visits:
        return None
    counts = sorted(visits.values(), reverse=True)
    half = sum(counts) / 2.0
    taken = 0
    running = 0
    for count in counts:
        running += count
        taken += 1
        if running >= half:
            break
    return taken * 100.0 / len(counts)


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    trampling = (sys.argv[2].lower() != "off") if len(sys.argv) > 2 else True
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-routes-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир и темп — как в остальных проверках, и по той же причине: сервер не
    # читает команд с подключения, которое сам заливает (см.
    # check_animal_delta.py), а наблюдателю здесь надо не отстать ни на шаг —
    # пропущенная дельта это пропущенные посещения.
    config["area"] = {"width": 96, "height": 96}
    # Тропы включаются и выключаются здесь, а не пересборкой: замер обязан
    # сравнивать один и тот же бинарник с собой (core/WorldToggles.hpp).
    config.setdefault("terrain", {}).setdefault("toggles", {})["trampling"] = trampling
    config["tick_interval_ms"] = 5
    config["snapshot_interval_ms"] = 1
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
        visits = collections.Counter()
        steps = 0
        # Слой утоптанности целиком, как его держит клиент: приходит один раз
        # в world_init и правится дельтами. Держать его надо ЖИВЫМ, а не
        # смотреть в конце: вопрос "по утоптанному ли шёл гоблин" задаётся о
        # том тике, когда он шагнул, а не о том, что осталось к концу прогона.
        trampled = []
        width = 0
        trail_steps = 0
        seen = {}       # id -> клетки, где он уже был
        walked = {}     # id -> сколько шагов сделал
        returned = {}   # id -> сколько из них были возвратами
        while tick < ticks:
            message = probe.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                tick = message.get("tick", tick)
                width = (message.get("area") or {}).get("width", width)
                trampled = list((message.get("layers") or {}).get("trampled") or [])
                continue
            if kind != "world_delta":
                continue
            tick = message.get("tick", tick)
            # Слой правится ДО разбора шагов этого же сообщения: и то, и
            # другое случилось в одном тике, а утоптанность, которую гоблин
            # видел, выбирая шаг, — та, что была к его началу.
            pairs = message.get("trampled") or []
            for i in range(0, len(pairs) - 1, 2):
                if pairs[i] < len(trampled):
                    trampled[pairs[i]] = pairs[i + 1]
            changes = message.get("goblins")
            if goblins is None or not changes:
                continue
            # Только шаги: где гоблин стоял, не двигаясь, нас не интересует —
            # вопрос про маршруты, а не про то, где он залежался.
            triples = changes.get("pos", [])
            for i in range(0, len(triples) - 2, 3):
                index = triples[i]
                if index < len(goblins):
                    goblin = goblins[index]
                    cell = (triples[i + 1], triples[i + 2])
                    goblin["x"], goblin["y"] = cell
                    visits[cell] += 1
                    steps += 1
                    if width and trampled:
                        flat = cell[1] * width + cell[0]
                        if flat < len(trampled) and trampled[flat] >= TRAIL:
                            trail_steps += 1
                    # Свой след, по каждому отдельно: возврат — это шаг на
                    # клетку, где ЭТОТ гоблин уже был.
                    own = seen.setdefault(goblin["id"], set())
                    walked[goblin["id"]] = walked.get(goblin["id"], 0) + 1
                    if cell in own:
                        returned[goblin["id"]] = returned.get(goblin["id"], 0) + 1
                    else:
                        own.add(cell)
            gone = set(changes.get("gone", []))
            if gone:
                goblins = [g for i, g in enumerate(goblins) if i not in gone]
            born = changes.get("born", [])
            if born:
                goblins = sorted(goblins + list(born), key=lambda g: g["id"])

        share = concentration(visits)
        if share is None:
            print("Гоблины не сделали ни шага — мерить нечего")
            return 1

        # Только те, кто успел походить: у новорождённого, сделавшего три
        # шага, доля возвратов ничего не значит и портит среднее.
        rates = [returned.get(key, 0) * 100.0 / count
                 for key, count in walked.items() if count >= 50]
        rates.sort()
        average = sum(rates) / len(rates) if rates else 0.0
        median = rates[len(rates) // 2] if rates else 0.0

        print(f"Тик {tick}, гоблинов {len(goblins or [])}, шагов {steps}, "
              f"побывали на {len(visits)} клетках")
        print(f"ВОЗВРАТЫ: {average:.1f}% шагов в среднем ведут туда, где гоблин уже был "
              f"(медиана {median:.1f}%, считано по {len(rates)} гоблинам)")
        print(f"  справка — сосредоточенность: половина шагов на {share:.1f}% клеток "
              f"(зависит от численности, сравнивать осторожно)")

        # Тропы. Сравнивать надо ДВЕ доли между собой: сколько земли
        # утоптано и какая доля шагов по ней сделана. Если вторая не выше
        # первой — притяжение не работает, и тропа осталась следом.
        land = sum(1 for value in trampled if value is not None)
        beaten = sum(1 for value in trampled if value >= TRAIL)
        deep = sum(1 for value in trampled if value >= 50)
        if land:
            print(f"ТРОПЫ: утоптано {beaten * 100.0 / land:.2f}% клеток "
                  f"(из них крепко, выше половины: {deep}), "
                  f"шагов по утоптанному {trail_steps * 100.0 / max(1, steps):.1f}%")
        top = visits.most_common(5)
        print("  самые хоженые: " + ", ".join(f"({x},{y}):{n}" for (x, y), n in top))
        return 0
    finally:
        server.terminate()
        server.wait(timeout=10)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

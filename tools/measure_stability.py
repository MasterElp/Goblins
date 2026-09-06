"""Замер устойчивости мира: держится ли пищевая цепь на длинном прогоне.

Все прочие проверки в этой папке спрашивают "не сломалось ли устройство" —
доезжает ли поле, сходится ли дельта, замыкается ли круг сохранения. Эта
спрашивает другое: **живёт ли мир**. Вымирают ли ярусы, выходит ли поголовье
на полку, не съедает ли стадо луг под ноль.

Зачем понадобилась отдельная. Мир мерился прогонами на 300 тиков, и этого
мало на порядки: с множителем долголетия взросление зверя наступает к
34 000 тикам, а полная жизнь доходит до 200 000. За триста тиков не
сменяется ни одно поколение — видно только, как оседает стартовый посев.
Всякий вывод об устойчивости, сделанный на такой длине, говорит о посеве, а
не о мире.

Инструмент, а не одноразовый скрипт: мерить придётся ещё не раз — после
всякой правки, которая трогает законы жизни.

Разговор с сервером устроен ровно так же, как в check_world_save.py, и это
не украшение, а условие работоспособности. Подключений ДВА: одно слушает,
второе командует. Сервер обслуживает соединение одним потоком, и пока тот
занят отправкой снимков, входящие кадры того же соединения он не читает —
просьба, поданная по залитому подключению, не доходит.

И слушающее подключение надо ЧИТАТЬ В ЦИКЛЕ, а не спать. Первый набросок
этого замера послал start_simulation и заснул на две минуты — мир остался на
тике 0. Причина: сервер шлёт снимки в сокет, их никто не забирает, очередь
отправки заполняется, и тик встаёт. Петля чтения не даёт очереди забиться и
заодно служит счётчиком тиков — другого источника номера тика у наблюдателя
нет.

Помощники сессии (connect/wait_for/settle) здесь свои, а не общие с
соседними проверками, и это не небрежность: в трёх существующих
инструментах они написаны тремя разными способами (класс с методами,
свободные функции, встроенный код). Свести их в один — отдельная работа,
которая переписывает три работающих проверки, и делать её заодно с замером
значило бы рисковать обеими.

Запуск (сервер поднимается сам):
    python tools/measure_stability.py --minutes 20 --seeds 101,202,303
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ws_probe import WebSocketProbe

from server_binary import find_server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 9111
WORLD = "stability"

# Насколько долго молчащий мир считается вставшим. Тик может замедлиться от
# нагрузки, но не онеметь: если за столько секунд номер тика не сдвинулся,
# дальше ждать нечего — надо сказать об этом, а не выдать короткий прогон за
# длинный.
STALL_SECONDS = 90.0


def connect(port):
    for _ in range(60):
        try:
            return WebSocketProbe(port=port)
        except OSError:
            time.sleep(1)
    return None


def wait_for(probe, kind, seconds=180):
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


def settle(probe, seconds=30.0, quiet=1.0):
    """Дочитать всё и дождаться тишины. Только на остановленном мире."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if probe.recv(quiet) is None:
            return True
    return False


def world_config(seed, port, saves_dir, area=None):
    """Копия настроек пользователя со своим портом и seed.

    Область берётся ЕГО, а не удобная маленькая: мерить надо тот мир, в
    который он играет. Тик просится самый частый, какой сервер даст, — замер
    ограничен временем, и каждая миллисекунда паузы это тики, которых не
    будет.

    Рассылка, наоборот, редкая: снимок нужен нам только как источник номера
    тика, а частая рассылка на карте в 26 000 клеток съедает то самое время,
    ради которого тик и ускорен.
    """
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    if area is not None:
        # Подмена размера нужна ровно для одного: сличить этот замер с
        # check_world_save.py, который гоняет мир 96x96. Совпасть они обязаны
        # на первых тех же тиках — иначе новый инструмент гоняет мир не так,
        # как все остальные, и мерить им нечего.
        config["area"] = {"width": area[0], "height": area[1]}
    config["port"] = port
    config["seed"] = seed
    config["tick_interval_ms"] = 0
    config["snapshot_interval_ms"] = 500
    config["saves_dir"] = saves_dir
    return config


def run_once(server_binary, seed, target_ticks, budget_seconds, workdir, area=None):
    """Один прогон. Возвращает (путь к файлу мира, сколько тиков вышло, секунд)."""
    config = world_config(seed, PORT, "saves", area)
    config_path = os.path.join(workdir, "config.json")
    json.dump(config, open(config_path, "w", encoding="utf-8"), indent=4, sort_keys=True)

    server = subprocess.Popen([server_binary, config_path], cwd=workdir,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        listen = connect(PORT)
        control = connect(PORT)
        if listen is None or control is None:
            print("Сервер не поднялся")
            return None, 0, 0.0

        # Командное подключение отворачивается сразу и навсегда: по залитому
        # подключению сервер команд не читает.
        control.send({"type": "updates", "enabled": False})
        while control.recv(1.0) is not None:
            pass

        control.send({"type": "start_simulation"})
        started = time.time()
        deadline = started + budget_seconds
        tick = 0
        moved_at = time.time()
        while tick < target_ticks and time.time() < deadline:
            message = listen.recv(5.0)
            if message is not None and message.get("type") in ("world_init", "world_delta"):
                got = message.get("tick", tick)
                if got > tick:
                    tick = got
                    moved_at = time.time()
            if time.time() - moved_at > STALL_SECONDS:
                print(f"  seed {seed}: мир встал на тике {tick}")
                break
        spent = time.time() - started

        control.send({"type": "stop_simulation"})
        settle(listen)
        control.send({"type": "save_world", "name": f"{WORLD}-{seed}"})
        notice = wait_for(listen, "notice", 300)
        if notice is None or "saved" not in notice.get("text", ""):
            print(f"  seed {seed}: мир не сохранился ({notice})")
            return None, tick, spent

        saved = os.path.join(os.path.dirname(server_binary), "saves", f"{WORLD}-{seed}.json")
        return (saved if os.path.exists(saved) else None), tick, spent
    finally:
        server.terminate()
        try:
            server.wait(timeout=15)
        except Exception:
            server.kill()


def protein_need(entity):
    return max(1, 6 * entity["animal_genome"]["adult_size"] // 1000)


def digest(path):
    """Что мир рассказывает о себе к концу прогона."""
    world = json.load(open(path, encoding="utf-8"))
    points = world["history"]["points"]
    entities = world["entities"]

    tiers = {}
    for label, key in (("травоядные", "herbivore"), ("хищники", "predator"), ("гоблины", "goblin")):
        alive = [e for e in entities if key in e and "animal_genome" in e]
        grown = [e for e in alive if e["animal"]["growth"] >= 900]
        ready = [e for e in grown if e["animal"]["protein"] >= protein_need(e)]
        tiers[label] = {
            "живых": len(alive),
            "здоровье": round(statistics.mean(e["animal"]["health"] for e in alive)) if alive else 0,
            "доросли": len(grown),
            "готовы плодиться": len(ready),
        }

    # Летопись: точка — плоский список, порядок задан протоколом (см.
    # "history" в server/NetworkServer.hpp). Числа тут позиции, и это
    # неприятно, но лучше, чем угадывать по длине: у травы и деревьев может
    # оказаться поровну видов, и тогда не отличить одно от другого.
    #
    # Средние геномы (позиции 4, 5, 6, 8, 10, 12) замеру не нужны: он про
    # то, живы ли ярусы, а не про то, куда поехала наследственность.
    tier_at = {"трава": 1, "травоядные": 2, "хищники": 3, "деревья": 7,
               "гоблины": 9, "кусты": 11}
    curve = []
    for point in points:
        row = {"тик": point[0]}
        for label, index in tier_at.items():
            row[label] = sum(point[index]) if index < len(point) else None
        curve.append(row)
    return {"tick": world["info"]["tick"], "curve": curve, "tiers": tiers,
            "interval": world["history"].get("interval")}


def extinctions(curve):
    """На каком тике ярус обнулился и больше не вернулся.

    Именно "и не вернулся": одиночный ноль в летописи — не вымирание, а
    мгновение между смертью последнего взрослого и рождением следующего.
    Считать вымиранием первый же ноль значило бы хоронить ярусы, которые
    живы.
    """
    out = {}
    for label in ("трава", "травоядные", "хищники", "деревья", "гоблины", "кусты"):
        gone = None
        for row in curve:
            value = row.get(label)
            if value is None:
                continue
            if value == 0:
                if gone is None:
                    gone = row["тик"]
            else:
                gone = None
        out[label] = gone
    return out


def report(seed, ticks, spent, data):
    print(f"\n--- seed {seed}: {ticks} тиков за {spent / 60:.1f} мин "
          f"({ticks / max(1e-9, spent):.0f} тиков/с) ---")
    curve = data["curve"]
    step = max(1, len(curve) // 12)
    # Последняя точка показывается всегда, даже если шаг через неё
    # перешагнул: конец прогона — это то, ради чего он и делался.
    shown = list(range(0, len(curve), step))
    if shown and shown[-1] != len(curve) - 1:
        shown.append(len(curve) - 1)
    columns = ("трава", "деревья", "кусты", "травоядные", "хищники", "гоблины")
    print("  тик      " + "".join(f"{name:>11}" for name in columns))
    for index in shown:
        row = curve[index]
        cells = "".join(f"{('—' if row.get(name) is None else row[name]):>11}" for name in columns)
        print(f"  {row['тик']:8d} {cells}")
    for label, gone in extinctions(curve).items():
        if gone is not None:
            print(f"  ВЫМЕРЛИ: {label}, тик {gone}")
    for label, state in data["tiers"].items():
        print(f"  {label}: " + ", ".join(f"{k} {v}" for k, v in state.items()))


def main():
    parser = argparse.ArgumentParser(description="Замер устойчивости мира на длинном прогоне")
    parser.add_argument("--seeds", default="101,202,303",
                        help="через запятую; несколько нужны, чтобы отличить закон от случайности одного мира")
    parser.add_argument("--ticks", type=int, default=50000,
                        help="сколько тиков хочется; порядок смены поколения зверей")
    parser.add_argument("--minutes", type=float, default=20.0, help="бюджет времени на один прогон")
    parser.add_argument("--out", default="", help="куда сложить разбор в JSON (необязательно)")
    parser.add_argument("--area", default="", help="ШxВ вместо размера из настроек (для сличения с check_world_save)")
    args = parser.parse_args()

    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    area = None
    if args.area:
        area = tuple(int(v) for v in args.area.lower().split("x"))

    import tempfile
    workdir = tempfile.mkdtemp(prefix="goblins-stability-")
    collected = {}
    for seed in [int(s) for s in args.seeds.split(",") if s.strip()]:
        path, ticks, spent = run_once(server_binary, seed, args.ticks, args.minutes * 60.0, workdir, area)
        if path is None:
            print(f"seed {seed}: прогон не дал файла мира")
            continue
        data = digest(path)
        collected[seed] = {"ticks": ticks, "seconds": spent, **data}
        report(seed, ticks, spent, data)

    if args.out and collected:
        json.dump(collected, open(args.out, "w", encoding="utf-8"), ensure_ascii=False)
        print(f"\nразбор сложен в {args.out}")
    return 0 if collected else 1


if __name__ == "__main__":
    sys.exit(main())

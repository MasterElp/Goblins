"""От чего гоблины умирают — числом, а не на глаз.

Мир только что перестал быть для гоблина безопасным: хищник его видит, и
смерть от зубов встала третьей рядом со старостью и истощением. Вопрос, ради
которого написана эта мера, ровно один и задан так, чтобы на него можно было
ответить "нет":

    **Численность поселения держат его собственные решения или хищник?**

Это тот самый вопрос, из-за которого зубы и были отложены
(docs/plan/10_Goblins_roadmap.md: "иначе численность поселенцев будет держать
хищник, а не их собственные решения"). Ответ на него — доля смертей от зубов
среди всех. Мало — мир прежний, зубы только приправа. Много — поселение живёт
не своей жизнью, и всё, что дальше строится на его решениях, строится зря.

Четыре числа, которые для этого считаются:

1. **Доли смертей** по трём причинам: зубы, старость, истощение.
2. **Развитость погибшего** в каждой доле. Проверяет предсказание, ради
   которого зубы и заводились: берут молодых и мелких, потому что хищник не
   трогает добычу крупнее семи десятых своего размера (kHuntPreyShare). Если
   от зубов гибнут ровно те же, кто и от старости, — отбора нет, и размер в
   охоте ничего не решает.
3. **Был ли хищник рядом** в момент смерти — по каждой доле. Это проверка
   САМОЙ МЕРЫ, а не мира: если "зубы" случаются там, где хищника нет в
   десяти клетках, значит причина названа неверно и остальным числам верить
   нельзя.
4. **Сколько гоблинов дожило до конца** — просто чтобы было видно, о каком
   поголовье шла речь.

Как отличается причина. Возраста на проводе нет, а здоровье есть, и три
причины пишут по нему три разных следа:

* зубы — обрыв: один удар снимает не меньше пятисот из тысячи
  (kStrikePerSize = 350 при отношении размеров от 1.43), то есть полсотни
  сотых за тик;
* истощение — сползание: голод и жажда отнимают 20 и 30 тысячных за тик, да
  ещё поделённые темпом жизни, — на проводе это единицы за десяток тиков;
* старость — обрыв в никуда: здоровье целое, и гоблин просто исчезает.

Порог между первым и вторым взят с большим запасом в обе стороны: десять
сотых за тик — это впятеро больше самого быстрого сползания и впятеро меньше
самого слабого укуса. Промахнуться им трудно, и третье число это проверяет.

Запуск:

    python3 tools/watch_goblin_deaths.py 6000

Второй прогон для сравнения делать не нужно, в отличие от нрава и троп: здесь
меряется не разница между двумя мирами, а состав одного.
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
PORT = 9113
TICKS = 6000

# Насколько здоровье должно упасть за один тик, чтобы это был укус. Сотые
# доли — так здоровье уходит на провод (shared/protocol/WirePrecision.hpp).
# Обоснование числа — в шапке.
BITE_DROP = 10

# Докуда считается, что хищник "был рядом". Не про закон мира, а про проверку
# самой меры: удар наносится вплотную (kStrikeReach), но между последней
# дельтой и смертью зверь успевает шагнуть, а гоблин — уйти в дельте позже.
# Десять клеток — заведомо больше любого такого запаздывания.
NEAR = 10


def connect():
    for _ in range(60):
        try:
            return WebSocketProbe(port=PORT)
        except OSError:
            time.sleep(1)
    return None


def apply_changes(creatures, changes):
    """То же самое, что делает NetworkClient::applyCreatureChanges.

    Списано из check_animal_delta.py, где оно машинно сверено с полным
    world_init. Считать по своему разбору значило бы мерить не тот мир,
    который видит клиент.

    Отличие одно: удалённые не выбрасываются молча, а возвращаются наружу —
    ради них мера и написана.
    """
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


def cause_of(drop, health):
    """Отчего погиб — по следу, который причина оставила в здоровье.

    drop — самое резкое падение за тик из виденных, health — последнее
    известное. Порядок вопросов важен: укус проверяется первым, потому что
    голодный гоблин тоже может быть съеден, и съеден он именно зубами.
    """
    if drop >= BITE_DROP:
        return "зубы"
    if health <= 2:
        return "истощение"
    return "старость"


def main():
    ticks = int(sys.argv[1]) if len(sys.argv) > 1 else TICKS
    server_binary = find_server(ROOT)
    if server_binary is None:
        print("Сервер не собран: ./build.sh")
        return 1

    workdir = tempfile.mkdtemp(prefix="goblins-deaths-")
    config = json.load(open(os.path.join(ROOT, "config.json"), encoding="utf-8"))
    config["port"] = PORT
    # Мир и темп — как в остальных проверках: тот же размер карты, чтобы
    # числа можно было класть рядом с числами watch_camps.py.
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
        animals = None
        tick = 0
        born_count = 0
        # Самое резкое падение здоровья, виденное у каждого. Хранится по
        # идентификатору, а не по месту в списке: места сдвигаются рождениями
        # и смертями, а имя в мире у гоблина одно.
        worst_drop = collections.Counter()
        last_health = {}
        deaths = []
        while tick < ticks:
            message = listen.recv(120)
            if message is None:
                print(f"Мир встал на тике {tick}")
                return 1
            kind = message.get("type")
            if kind == "world_init":
                goblins = sorted(message.get("goblins") or [], key=lambda g: g["id"])
                animals = sorted(message.get("animals") or [], key=lambda a: a["id"])
                for goblin in goblins:
                    last_health[goblin["id"]] = goblin.get("health", 0)
                tick = message.get("tick", tick)
                continue
            if kind != "world_delta" or goblins is None:
                continue
            tick = message.get("tick", tick)

            # Хищники — по положениям ПРОШЛОГО тика, до применения этой
            # дельты: гоблин в ней уже мёртв, а зверь ещё стоит там, откуда
            # ударил.
            predators = [(a["x"], a["y"]) for a in animals if a.get("kind") == "predator"]

            if "animals" in message:
                animals, _ = apply_changes(animals, message["animals"])
            changes = message.get("goblins")
            if not changes:
                continue
            before = {g["id"]: g.get("health", 0) for g in goblins}
            goblins, gone = apply_changes(goblins, changes)
            born_count += len(changes.get("born", []))
            for goblin in goblins:
                was = before.get(goblin["id"])
                now = goblin.get("health", 0)
                if was is not None and was - now > worst_drop[goblin["id"]]:
                    worst_drop[goblin["id"]] = was - now
                last_health[goblin["id"]] = now
            for dead in gone:
                # Падение в том самом тике, когда гоблин исчез, в общий счёт
                # выше не попадает: его уже нет в списке. Считается здесь.
                was = before.get(dead["id"], dead.get("health", 0))
                drop = max(worst_drop[dead["id"]], was - dead.get("health", 0))
                near = min((max(abs(px - dead["x"]), abs(py - dead["y"])) for px, py in predators),
                           default=NEAR + 1)
                deaths.append({
                    "cause": cause_of(drop, dead.get("health", 0)),
                    "growth": dead.get("growth", 0),
                    "near": near <= NEAR,
                })

        control.send({"type": "stop_simulation"})

        if not deaths:
            print(f"За {ticks} тиков не умер никто — мерить нечего "
                  f"(живых {len(goblins)}, родилось {born_count})")
            return 0

        print(f"Смертей за {ticks} тиков: {len(deaths)}. "
              f"Живых к концу {len(goblins)}, родилось {born_count}.")
        for cause in ("зубы", "старость", "истощение"):
            rows = [d for d in deaths if d["cause"] == cause]
            if not rows:
                print(f"  {cause}: ни одной")
                continue
            share = 100.0 * len(rows) / len(deaths)
            growth = sum(r["growth"] for r in rows) / len(rows)
            near = 100.0 * sum(1 for r in rows if r["near"]) / len(rows)
            print(f"  {cause}: {len(rows)} ({share:.0f}%), "
                  f"развитость при смерти {growth:.0f}, хищник рядом у {near:.0f}%")

        teeth = 100.0 * sum(1 for d in deaths if d["cause"] == "зубы") / len(deaths)
        print(f"Численность держит: {'ХИЩНИК' if teeth >= 50 else 'поселение'} "
              f"({teeth:.0f}% смертей от зубов)")
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

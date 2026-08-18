#!/usr/bin/env python3
"""Проверка, что настройки генерации доезжают по всему кругу.

Круг такой: config.json -> ServerConfig -> RegenerationRequest -> панель
клиента -> обратно в RegenerationRequest -> ServerConfig -> config.json.
Параметр, потерянный на любом из переходов, ведёт себя одинаково скверно:
ползунок двигается, мир не меняется, файл не меняется, и никто не ругается.

Так уже случалось дважды. Секция животных появилась в конфигурации, в
панели и в протоколе — но её забыли дописать в два места, которые
перекладывают поля, и настройки поголовья не доезжали НИКУДА: панель
показывала умолчания, мир рождался с умолчаниями, "Save values" молча
выбрасывал правки. До того тем же кончилась настройка растений
("Не сохранялись параметры" в CHANGELOG).

Поэтому проверка машинная, а не глазами: каждому полю даётся своё, ни на
что не похожее значение, потом каждое меняется ещё раз и сохраняется.
Потерянное поле называется по имени.

Запуск (сервер собран, порт 9002 свободен):
    python3 tools/check_config_roundtrip.py [путь/к/server]
"""
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ws_probe import WebSocketProbe

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Значения внутри допустимых диапазонов ползунков, но заведомо не умолчания:
# совпадение с умолчанием скрыло бы ровно ту ошибку, которую ищем.
PROBE = {
    "area": {"width": 96, "height": 112},
    "seed": 777333,
    "boulder_count": 23,
    "terrain": {
        "feature_size": 37, "noise_octaves": 6, "mountain_height": 21000,
        "mountain_hardness": 430, "river_count": 4, "river_width": 44,
        "river_sinuosity": 310, "river_depth": 1234, "pond_depth": 1700,
        "minerals_average": 17, "water_source_count": 6, "water_source_depth": 3300,
        "water_evaporation_rate": 77, "rain_interval_ticks": 555, "rain_amount": 66,
        "soil_erosion_rate": 88,
    },
    "plants": {"grass_species": 9, "grass_coverage": 133, "mutation_rate": 202,
               "humus_decay_period": 27},
    "animals": {"herbivore_species": 5, "predator_species": 4,
                "herbivore_count": 211, "predator_count": 31, "mutation_rate": 175},
}


def flat(value, prefix=""):
    out = {}
    for key, item in value.items():
        if isinstance(item, dict):
            out.update(flat(item, prefix + key + "."))
        else:
            out[prefix + key] = item
    return out


def main():
    server = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "server", "server")
    if not os.path.exists(server):
        print(f"Сервер не найден: {server}. Соберите его (./build.sh) или укажите путь.")
        return 2

    config = json.load(open(os.path.join(ROOT, "config.json")))
    config.update(PROBE)
    path = os.path.join(tempfile.mkdtemp(prefix="goblins-roundtrip-"), "config.json")
    json.dump(config, open(path, "w"), indent=4)

    failures = []
    process = subprocess.Popen([server, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(3)
        ws = WebSocketProbe()
        generation = None
        for _ in range(20):
            message = ws.recv()
            if message.get("type") == "world_init":
                generation = message["generation"]
                break
        if generation is None:
            print("Сервер не прислал world_init.")
            return 2

        # --- 1. config.json -> панель ---
        arrived = {
            "area": {"width": generation["area_width"], "height": generation["area_height"]},
            "seed": generation["seed"],
            "boulder_count": generation["boulder_count"],
            "terrain": generation["terrain"],
            "plants": generation["plants"],
            "animals": generation["animals"],
        }
        arrived_flat = flat(arrived)
        for name, expected in flat(PROBE).items():
            if arrived_flat.get(name) != expected:
                failures.append(f"config.json -> панель: {name} = {arrived_flat.get(name)}, ожидалось {expected}")

        # --- 2. панель -> config.json ---
        # Правим КАЖДОЕ поле: потерянное не изменится в файле.
        edited = json.loads(json.dumps(generation))
        expected_saved = {}
        for section in ("terrain", "plants", "animals"):
            for name in edited[section]:
                edited[section][name] += 10
                expected_saved[f"{section}.{name}"] = edited[section][name]
        for name, key in (("boulder_count", "boulder_count"), ("seed", "seed")):
            edited[name] += 1 if name == "seed" else 10
            expected_saved[key] = edited[name]
        edited["area_width"] += 10
        edited["area_height"] += 10
        expected_saved["area.width"] = edited["area_width"]
        expected_saved["area.height"] = edited["area_height"]

        ws.send({"type": "save_generation_config", "params": edited})
        for _ in range(40):
            message = ws.recv()
            if message.get("type") == "notice" and "saved" in message.get("text", ""):
                break
        time.sleep(0.5)
    finally:
        process.terminate()
        process.wait(timeout=10)

    saved = flat(json.load(open(path)))
    for name, expected in expected_saved.items():
        if saved.get(name) != expected:
            failures.append(f"панель -> config.json: {name} = {saved.get(name)}, ожидалось {expected}")

    total = len(flat(PROBE)) + len(expected_saved)
    if failures:
        print(f"ПРОВАЛ: {len(failures)} из {total} проверок")
        for failure in failures:
            print("  -", failure)
        return 1
    print(f"Круг настроек замкнут: {total} проверок, все поля доезжают в обе стороны")
    return 0


if __name__ == "__main__":
    sys.exit(main())

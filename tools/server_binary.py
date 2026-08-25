"""Где лежит собранный сервер — один ответ на все проверки.

Собирается он в разные места: на Linux это `build/server/server`, в
Visual Studio — `build/server/Release/server.exe` рядом с `Debug/server.exe`,
а у того, кто держит две конфигурации, их и вовсе несколько. Проверкам нужен
один, и обязательно ТОТ, который только что собрали.

Берётся самый свежий по времени записи, а не первый найденный. Прежде каждая
проверка знала жёсткий путь `build/server/server`, которого на Windows не
существует, и чтобы они работали, туда клали копию руками. Копия оставалась
лежать, устаревала, и `run.sh` — искавший тем же "первый попавшийся" —
однажды запустил её вместо свежей сборки: мир поднимался без всего, что было
сделано за день, выглядел при этом совершенно исправным, и причину искали в
коде, который к делу не относился.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_server(root=None):
    """Путь к самому свежему собранному серверу или None."""
    root = root or ROOT
    found = []
    # Оба каталога сборки: обычный и тот, что заводят рядом, чтобы не
    # перетирать первый.
    for build in ("build", "build-rel"):
        base = os.path.join(root, build, "server")
        if not os.path.isdir(base):
            continue
        for directory, _, files in os.walk(base):
            for name in files:
                if name in ("server", "server.exe"):
                    path = os.path.join(directory, name)
                    found.append((os.path.getmtime(path), path))
    if not found:
        return None
    found.sort(reverse=True)
    return found[0][1]

#!/usr/bin/env bash
# Запуск сервера и клиента. На Windows — из Git Bash или WSL.
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Самый свежий из найденных, а не первый попавшийся.
#
# В каталоге сборки их со временем заводится несколько: Debug рядом с
# Release, остатки прежних конфигураций. Прежний "head -n 1" брал тот,
# который первым отдала файловая система, — и это оказывался Debug
# многодневной давности. Запускался мир без того, что было сделано за эти
# дни, выглядел он при этом совершенно исправным миром, и искать причину
# приходилось в коде, который к делу не относился вовсе.
find_binary() {
    local search_dir="$1"
    local name="$2"
    find "$search_dir" -type f \( -name "$name" -o -name "$name.exe" \) -printf '%T@ %p
' 2>/dev/null |
        sort -rn | head -n 1 | cut -d' ' -f2-
}

SERVER_BIN="$(find_binary "$ROOT_DIR/build/server" "server")"
CLIENT_BIN="$(find_binary "$ROOT_DIR/clients/desktop/build" "client")"

if [ -z "$SERVER_BIN" ]; then
    echo "Server binary not found. Run ./build.sh first."
    exit 1
fi

if [ -z "$CLIENT_BIN" ]; then
    echo "Client binary not found. Run ./build.sh first."
    exit 1
fi

# Сервер — со своим config.json из корня репозитория.
echo "Starting server: $SERVER_BIN $ROOT_DIR/config.json"
"$SERVER_BIN" "$ROOT_DIR/config.json" &
SERVER_PID=$!

cleanup() {
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        echo ""
        echo "Stopping server (pid $SERVER_PID)..."
        kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Даём серверу время поднять WebSocket-порт перед подключением клиента.
sleep 1

# Клиент — без аргумента: сам найдёт (или создаст) свой config.json рядом
# со своим исполняемым файлом.
echo "Starting client: $CLIENT_BIN"
"$CLIENT_BIN"

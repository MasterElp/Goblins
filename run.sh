#!/usr/bin/env bash
# Linux/macOS. На Windows используй run.ps1 (см. build.sh про причину).
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_PATH="${1:-$ROOT_DIR/config.json}"

find_binary() {
    local search_dir="$1"
    local name="$2"
    find "$search_dir" -type f \( -name "$name" -o -name "$name.exe" \) 2>/dev/null | head -n 1
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

echo "Starting server: $SERVER_BIN $CONFIG_PATH"
"$SERVER_BIN" "$CONFIG_PATH" &
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

echo "Starting client: $CLIENT_BIN $CONFIG_PATH"
"$CLIENT_BIN" "$CONFIG_PATH"

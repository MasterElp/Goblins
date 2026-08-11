#!/usr/bin/env bash
# Linux/macOS. На Windows используй build.bat — та же сборка, но без
# зависимости от bash (Git Bash/WSL) и без вопросов политики выполнения
# скриптов, характерных для PowerShell.
set -e

CONFIG="${1:-Release}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "== Building server (config: $CONFIG) =="
cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$ROOT_DIR/build" --config "$CONFIG" -j

echo ""
echo "== Building client (config: $CONFIG) =="
cmake -S "$ROOT_DIR/clients/desktop" -B "$ROOT_DIR/clients/desktop/build" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$ROOT_DIR/clients/desktop/build" --config "$CONFIG" -j

echo ""
echo "Done."

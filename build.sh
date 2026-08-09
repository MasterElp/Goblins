#!/usr/bin/env bash
# Linux/macOS. На Windows используй build.ps1 — интерактивный клиент
# (conio.h/ANSI) рассчитан на настоящую консоль Windows, а не на mintty
# из Git Bash, поэтому сборка отдельно от запуска тут не проблема, а вот
# для run.sh на Windows это важно (см. run.ps1).
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

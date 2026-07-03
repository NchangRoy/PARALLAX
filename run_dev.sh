#!/usr/bin/env bash
# Launches the Flask backend and the Vite frontend together for local dev.
# Ctrl+C stops both.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$ROOT_DIR/backend_ui_parallax"
FRONTEND_DIR="$ROOT_DIR/Parallax-Portal"

pids=()
cleanup() {
    echo "Stopping..."
    for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

(
    cd "$BACKEND_DIR"
    source .venv/bin/activate
    exec python run.py
) &
pids+=($!)

(
    cd "$FRONTEND_DIR"
    exec npm run dev
) &
pids+=($!)

wait

#!/usr/bin/env bash
# (thx for getting me a quick playground area claude)
#
# Build and run a playground target.
# Usage:  ./playground/b-test-luau.sh [target] [args...]
#   target  — name of the .cpp file without extension (default: test-luau)
#   args    — forwarded to the executable
#
# Run from the repo root OR from inside the playground/ directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$ROOT/build"

TARGET="${1:-test-luau}"
shift 2>/dev/null || true   # remaining args go to the binary

# ── Configure ────────────────────────────────────────────────────────────────
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    echo "[playground] Configuring build..."
    cmake -S "$ROOT" -B "$BUILD" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DGENERATE_DOC=OFF
fi

# ── Build ─────────────────────────────────────────────────────────────────────
echo "[playground] Building target: $TARGET"
ninja -C "$BUILD" "$TARGET"

# ── Run ───────────────────────────────────────────────────────────────────────
BINARY="$BUILD/playground/$TARGET"
echo "[playground] Running: $BINARY $*"
echo "─────────────────────────────────────────"
exec "$BINARY" "$@"

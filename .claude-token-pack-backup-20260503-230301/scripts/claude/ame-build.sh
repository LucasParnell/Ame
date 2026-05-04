#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

BUILD_DIR="${AME_BUILD_DIR:-build}"
TARGET="${1:-}"
shift || true

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake 2>&1 \
    | python3 scripts/claude/ame-filter-build.py 220
fi

if [ -n "$TARGET" ]; then
  cmake --build "$BUILD_DIR" --target "$TARGET" -- "$@" 2>&1 \
    | python3 scripts/claude/ame-filter-build.py 260
else
  cmake --build "$BUILD_DIR" -- "$@" 2>&1 \
    | python3 scripts/claude/ame-filter-build.py 260
fi

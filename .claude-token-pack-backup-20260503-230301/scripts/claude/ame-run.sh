#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

SECONDS_LIMIT="${AME_QEMU_TIMEOUT:-35}"
CMD=(scripts/run-qemu.sh "$@")

if command -v timeout >/dev/null 2>&1; then
  DANGO_GUEST_TERMINAL=0 timeout "$SECONDS_LIMIT" "${CMD[@]}" 2>&1 \
    | python3 scripts/claude/ame-filter-build.py 240
else
  DANGO_GUEST_TERMINAL=0 "${CMD[@]}" 2>&1 \
    | python3 scripts/claude/ame-filter-build.py 240
fi

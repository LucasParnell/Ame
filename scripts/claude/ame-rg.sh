#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

if [ "$#" -lt 1 ]; then
  echo "usage: scripts/claude/ame-rg.sh <pattern> [extra rg args...]" >&2
  exit 2
fi

pattern="$1"
shift || true

rg --line-number --hidden --smart-case --max-columns 220 --max-count 80 \
  --glob '!/.git/**' \
  --glob '!build/**' --glob '!build-*/**' --glob '!out/**' --glob '!cmake-build-*/**' \
  --glob '!linux-artifacts/**' --glob '!docs/**' \
  --glob '!*.zip' --glob '!*.zip.b64' --glob '!*.b64' --glob '!*.log' \
  --glob '!*.patch' --glob '!*.rej' \
  --glob '!Midix_Architecture_3.md' --glob '!sched_spec_v_2.md' \
  "$@" -- "$pattern" \
  CMakeLists.txt boot kernel userland lib scripts 2>/dev/null | head -300

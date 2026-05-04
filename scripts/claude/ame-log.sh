#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
file="${1:-build/dango-guest.log}"
if [ ! -f "$file" ]; then
  echo "missing log: $file" >&2
  exit 1
fi
printf '%s\n' "--- critical lines from $file ---"
rg -n -i 'panic|fault|abort|assert|error|warn|exception|trace|svc|syscall|doorbell|irq|timer' "$file" | tail -160 || true
printf '%s\n' "--- last 80 lines ---"
tail -80 "$file"

#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

echo "== top directories/files by size =="
du -ah . --exclude='./.git' 2>/dev/null | sort -hr | head -35

echo
echo "== visible relevant file count =="
scripts/claude/ame-files.sh | wc -l

echo
echo "== generated/noisy roots if present =="
for p in build out .git linux-artifacts docs; do
  if [ -e "$p" ]; then du -sh "$p"; fi
done

echo
echo "== noisy files at repo root =="
find . -maxdepth 1 -type f \( -name '*.zip' -o -name '*.b64' -o -name '*.log' -o -name '*.patch' -o -name '*.rej' -o -name '*Architecture*.md' -o -name '*spec*.md' \) -printf '%p %s bytes\n' | sort

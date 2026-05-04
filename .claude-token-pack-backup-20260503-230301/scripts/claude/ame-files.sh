#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

if command -v fd >/dev/null 2>&1; then
  fd --type f \
    --exclude .git --exclude build --exclude 'build-*' --exclude out --exclude 'cmake-build-*' \
    --exclude linux-artifacts --exclude docs \
    --exclude '*.zip' --exclude '*.zip.b64' --exclude '*.b64' --exclude '*.log' \
    --exclude '*.patch' --exclude '*.rej' --exclude '*.o' --exclude '*.a' --exclude '*.elf' \
    . CMakeLists.txt boot kernel userland lib scripts \
    | sort
else
  find CMakeLists.txt boot kernel userland lib scripts -type f \
    ! -path '*/.git/*' ! -path 'build/*' ! -path 'out/*' ! -path 'linux-artifacts/*' ! -path 'docs/*' \
    ! -name '*.zip' ! -name '*.zip.b64' ! -name '*.b64' ! -name '*.log' ! -name '*.patch' ! -name '*.rej' \
    2>/dev/null | sort
fi

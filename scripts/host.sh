#!/usr/bin/env bash
# Shared host support; compatible with macOS's Bash 3.2 and Linux.
ame_jobs() {
    if [[ -n "${AME_BUILD_JOBS:-}" ]]; then
        printf '%s\n' "${AME_BUILD_JOBS}"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    elif [[ "$(uname -s)" == Darwin ]]; then
        sysctl -n hw.logicalcpu
    else
        printf '4\n'
    fi
}

ame_host_tools() {
    local source_root
    source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    if [[ -x "${source_root}/.venv/bin/python3" ]]; then
        export PATH="${source_root}/.venv/bin:${PATH}"
    fi
    if [[ "$(uname -s)" == Darwin ]] && command -v brew >/dev/null 2>&1; then
        local prefix
        prefix="$(brew --prefix)"
        export PATH="${AME_LLVM_ROOT:-${prefix}/opt/llvm}/bin:${prefix}/opt/lld/bin:${prefix}/opt/bison/bin:${prefix}/opt/flex/bin:${prefix}/opt/gptfdisk/sbin:${PATH}"
    fi
}

ame_file_size() {
    python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$1"
}

ame_sparse_file() {
    python3 - "$1" "$2" <<'PY'
import re
import sys
from pathlib import Path
match = re.fullmatch(r'([0-9]+)([KMGTP]?)(?:i?B)?', sys.argv[2], re.I)
if not match:
    raise SystemExit('invalid disk size: ' + sys.argv[2])
size = int(match[1]) * 1024 ** ('KMGTP'.index(match[2].upper()) + 1 if match[2] else 0)
with Path(sys.argv[1]).open('wb') as disk:
    disk.truncate(size)
PY
}

#!/usr/bin/env bash
set -euo pipefail

FILE="userland/init/main.cc"
cp "$FILE" "$FILE.bak.direct-console-log.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("userland/init/main.cc")
s = p.read_text()

old = '''static void console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}
'''

new = '''static void console_log(const char* msg) {
    // DEBUG: bypass userspace log ring / io.console while debugging boot hangs.
    // The DBG markers prove sys_debug_puts itself is working.
    sys_debug_puts(msg, slen(msg));
}
'''

if old not in s:
    raise SystemExit("could not find console_log()")

p.write_text(s.replace(old, new, 1))
PY

cmake --build build

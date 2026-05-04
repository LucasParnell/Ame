#!/usr/bin/env bash
set -euo pipefail

FILE="userland/init/main.cc"
cp "$FILE" "$FILE.bak.supervision-sys-kill.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("userland/init/main.cc")
s = p.read_text()

old = '''    // Supervision demo: force a crash message to pong, wait for a supervisor
    // death event, then restart according to the manifest policy.
    debug_marker("DBG init: supervision block entered\\n");
    uint32_t crash = MSG_CRASH;
    if (ping_to_pong_ring) {
        while (!ring_push(ping_to_pong_ring, kRingOrder, MSG_CRASH, &crash, sizeof(crash), db_pong)) {
            sys_yield();
        }
        debug_marker("DBG init: crash message pushed\\n");
    }
    bool saw_death = false;
'''

new = '''    // Supervision demo: kill pong through the process/supervisor path.
    //
    // Do not inject MSG_CRASH through ping_to_pong_ring here.  That ring is
    // SPSC and belongs to demo.ping -> demo.pong; using it from init as a
    // second producer makes the supervision test nondeterministic.
    debug_marker("DBG init: supervision block entered\\n");
    if (pong_proc == kNullCap) {
        fail_halt("init: supervision failed\\n");
    }
    sys_kill(pong_proc);
    debug_marker("DBG init: pong killed\\n");

    bool saw_death = false;
'''

if old not in s:
    raise SystemExit("could not find instrumented MSG_CRASH supervision block")

p.write_text(s.replace(old, new, 1))
PY

cmake --build build

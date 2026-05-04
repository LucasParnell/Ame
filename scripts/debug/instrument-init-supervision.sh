#!/usr/bin/env bash
set -euo pipefail

FILE="userland/init/main.cc"
cp "$FILE" "$FILE.bak.supervision-markers.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("userland/init/main.cc")
s = p.read_text()

def put_marker_func():
    return r'''
static void debug_marker(const char* msg) {
    sys_debug_puts(msg, slen(msg));
}
'''

if "static void debug_marker" not in s:
    s = s.replace(
        "static void console_log(const char* msg) {\n",
        put_marker_func() + "\nstatic void console_log(const char* msg) {\n",
        1,
    )

s = s.replace(
'''    // Supervision demo: force a crash message to pong, wait for a supervisor
    // death event, then restart according to the manifest policy.
    uint32_t crash = MSG_CRASH;
''',
'''    // Supervision demo: force a crash message to pong, wait for a supervisor
    // death event, then restart according to the manifest policy.
    debug_marker("DBG init: supervision block entered\\n");
    uint32_t crash = MSG_CRASH;
''',
1)

s = s.replace(
'''        while (!ring_push(ping_to_pong_ring, kRingOrder, MSG_CRASH, &crash, sizeof(crash), db_pong)) {
            sys_yield();
        }
    }
    bool saw_death = false;
''',
'''        while (!ring_push(ping_to_pong_ring, kRingOrder, MSG_CRASH, &crash, sizeof(crash), db_pong)) {
            sys_yield();
        }
        debug_marker("DBG init: crash message pushed\\n");
    }
    bool saw_death = false;
''',
1)

s = s.replace(
'''        if (event.value > 0) {
            saw_death = true;
            break;
        }
        sys_yield();
    }
    if (!saw_death || !pong || !token_eq(pong->restart, "always")) {
        fail_halt("init: supervision failed\\n");
    }

    if (!spawn_service(*pong, SPAWN_SUSPENDED)) fail_halt("init: supervision failed\\n");
''',
'''        if (event.value > 0) {
            debug_marker("DBG init: supervisor event received\\n");
            saw_death = true;
            break;
        }
        sys_yield();
    }
    if (!saw_death || !pong || !token_eq(pong->restart, "always")) {
        debug_marker("DBG init: supervision poll failed\\n");
        fail_halt("init: supervision failed\\n");
    }

    debug_marker("DBG init: respawning pong\\n");
    if (!spawn_service(*pong, SPAWN_SUSPENDED)) fail_halt("init: supervision failed\\n");
''',
1)

s = s.replace(
'''        require_ok(sys_thread_resume(pong->main_thread), "init: supervision failed\\n");
    } else {
        fail_halt("init: supervision failed\\n");
    }
    console_log("init: supervision ready\\n");
''',
'''        require_ok(sys_thread_resume(pong->main_thread), "init: supervision failed\\n");
        debug_marker("DBG init: pong resumed\\n");
    } else {
        fail_halt("init: supervision failed\\n");
    }
    console_log("init: supervision ready\\n");
''',
1)

p.write_text(s)
PY

cmake --build build

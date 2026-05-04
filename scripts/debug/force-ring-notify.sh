#!/usr/bin/env bash
set -euo pipefail

FILE="userland/lib/include/mochi/ring.h"
cp "$FILE" "$FILE.bak.force-notify.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("userland/lib/include/mochi/ring.h")
s = p.read_text()

old = '''    const uint32_t claimed_sleep =
        __atomic_exchange_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL);
    if (claimed_sleep) {
        (void)sys_notify(doorbell_cap);
    }

    return true;
'''

new = '''    const uint32_t claimed_sleep =
        __atomic_exchange_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL);

    // DEBUG: force notification on every published slot.
    //
    // This proves whether the tier-1 sleep-flag optimization is losing a wake.
    // It is intentionally noisier/slower, but should make the cross-core
    // doorbell phase impossible to deadlock if the ring contents are correct.
    (void)claimed_sleep;
    (void)sys_notify(doorbell_cap);

    return true;
'''

if old not in s:
    raise SystemExit("could not find ring_push notify block")

p.write_text(s.replace(old, new, 1))
PY

cmake --build build

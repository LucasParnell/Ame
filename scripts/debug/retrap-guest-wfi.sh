#!/usr/bin/env bash
set -euo pipefail

EL2="kernel/arch/arm64/el2.S"
cp "$EL2" "$EL2.bak.retrap-wfi.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("kernel/arch/arm64/el2.S")
s = p.read_text()

old = '''    // Do not trap guest WFI/WFE here.  With IMO/FMO/AMO set, host physical
    // IRQs still route to EL2 and wake the guest run.  Trapping guest idle
    // creates a tight WFx-exit loop once Linux reaches idle.
'''

new = '''    // Trap guest WFI/WFE.  The VMM currently injects virtual timer state from
    // the sys_vcpu_run() loop; if the guest is allowed to sleep in WFI, it can
    // stop before the agent/init path runs.  The C++ WFx handler advances PC
    // and returns to the async runner instead of spinning in one syscall.
    orr     x0, x0, #(1 << 13)   // TWI: trap guest WFI
    orr     x0, x0, #(1 << 14)   // TWE: trap guest WFE
'''

if old not in s:
    raise SystemExit("could not find the disabled WFI/WFE comment block in el2.S")

p.write_text(s.replace(old, new, 1))
PY

cmake --build build

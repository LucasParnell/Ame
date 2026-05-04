#!/usr/bin/env bash
set -euo pipefail

ENTRY="kernel/arch/arm64/entry.S"
EL2="kernel/arch/arm64/el2.S"

cp "$ENTRY" "$ENTRY.bak.el2-percpu-stack.$(date +%Y%m%d-%H%M%S)"
cp "$EL2" "$EL2.bak.el2-percpu-stack.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

entry = Path("kernel/arch/arm64/entry.S")
s = entry.read_text()

old_primary = """    adrp    x0, __el2_stack_top
    add     x0, x0, :lo12:__el2_stack_top
    mov     sp, x0
"""

new_primary = """    // Per-CPU EL2 stack.  EL2 can be entered concurrently on SMP guests,
    // especially while Dango vCPU runner threads are active.
    adrp    x0, g_el2_stacks
    add     x0, x0, :lo12:g_el2_stacks
    mrs     x1, MPIDR_EL1
    and     x1, x1, #0xff
    cmp     x1, #4
    b.lo    0f
    mov     x1, xzr
0:
    add     x1, x1, #1
    lsl     x1, x1, #14
    add     x0, x0, x1
    mov     sp, x0
"""

old_secondary = """    adrp    x1, __el2_stack_top
    add     x1, x1, :lo12:__el2_stack_top
    mov     sp, x1
"""

new_secondary = """    // Per-CPU EL2 stack for secondary CPUs too.  Do not share EL2 SP.
    adrp    x1, g_el2_stacks
    add     x1, x1, :lo12:g_el2_stacks
    mov     x2, x19
    cmp     x2, #4
    b.lo    7f
    mov     x2, xzr
7:
    add     x2, x2, #1
    lsl     x2, x2, #14
    add     x1, x1, x2
    mov     sp, x1
"""

if old_primary not in s:
    raise SystemExit("could not find primary __el2_stack_top setup")
s = s.replace(old_primary, new_primary, 1)

if old_secondary not in s:
    raise SystemExit("could not find secondary __el2_stack_top setup")
s = s.replace(old_secondary, new_secondary, 1)

entry.write_text(s)

el2 = Path("kernel/arch/arm64/el2.S")
s = el2.read_text()

old_stack = """.align 12
.global __el2_stack_top
__el2_stack_bottom:
    .skip 16384
__el2_stack_top:
"""

new_stack = """.align 14
.global g_el2_stacks
g_el2_stacks:
    // 4 CPUs * 16 KiB.  Stack top for CPU n is:
    //   g_el2_stacks + (n + 1) * 16384
    .skip 65536

// Compatibility symbol: top of the whole EL2 stack area.
.global __el2_stack_top
__el2_stack_top:
"""

if old_stack not in s:
    raise SystemExit("could not find single EL2 stack block")
s = s.replace(old_stack, new_stack, 1)

el2.write_text(s)
PY

cmake --build build

echo
echo "patched EL2 per-CPU stacks"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

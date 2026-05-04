#!/usr/bin/env bash
set -euo pipefail

ENTRY="kernel/arch/arm64/entry.S"
EL2="kernel/arch/arm64/el2.S"

[[ -f "$ENTRY" ]] || { echo "error: $ENTRY not found" >&2; exit 1; }
[[ -f "$EL2" ]] || { echo "error: $EL2 not found" >&2; exit 1; }

cp "$ENTRY" "$ENTRY.bak.el2-percpu-stack.$(date +%Y%m%d-%H%M%S)"
cp "$EL2" "$EL2.bak.el2-percpu-stack.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path
import re
import sys

entry_p = Path("kernel/arch/arm64/entry.S")
el2_p = Path("kernel/arch/arm64/el2.S")

entry = entry_p.read_text()
el2 = el2_p.read_text()

primary_old = """    adrp    x0, __el2_stack_top
    add     x0, x0, :lo12:__el2_stack_top
    mov     sp, x0
"""

primary_new = """    // Per-CPU EL2 stack.  EL2 can be entered concurrently on SMP.
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

secondary_old = """    adrp    x1, __el2_stack_top
    add     x1, x1, :lo12:__el2_stack_top
    mov     sp, x1
"""

secondary_new = """    // Per-CPU EL2 stack for secondary CPUs too.
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

if primary_old in entry:
    entry = entry.replace(primary_old, primary_new, 1)
    print("patched primary EL2 stack setup")
elif "adrp    x0, g_el2_stacks" in entry:
    print("primary EL2 stack setup already patched")
else:
    print("error: could not find primary EL2 stack setup in entry.S", file=sys.stderr)
    sys.exit(1)

if secondary_old in entry:
    entry = entry.replace(secondary_old, secondary_new, 1)
    print("patched secondary EL2 stack setup")
elif "adrp    x1, g_el2_stacks" in entry:
    print("secondary EL2 stack setup already patched")
else:
    print("error: could not find secondary EL2 stack setup in entry.S", file=sys.stderr)
    sys.exit(1)

entry_p.write_text(entry)

if ".global g_el2_stacks" in el2:
    print("EL2 stack storage already patched")
else:
    # Match the actual single-stack block, allowing whitespace/alignment changes.
    pat = re.compile(
        r"""
        \n\.align\s+\d+\s*\n
        \.global\s+__el2_stack_top\s*\n
        __el2_stack_bottom:\s*\n
        \s*\.skip\s+\d+\s*\n
        __el2_stack_top:\s*
        """,
        re.VERBOSE,
    )

    repl = """
.align 14
.global g_el2_stacks
g_el2_stacks:
    // 4 CPUs * 16 KiB.  Stack top for CPU n is:
    //   g_el2_stacks + (n + 1) * 16384
    .skip 65536

// Compatibility symbol: top of the whole EL2 stack area.
.global __el2_stack_top
__el2_stack_top:
"""

    el2_new, n = pat.subn(repl, el2, count=1)
    if n != 1:
        print("error: could not find single EL2 stack storage block in el2.S", file=sys.stderr)
        idx = el2.find("__el2_stack")
        if idx >= 0:
            print("\nnear __el2_stack:\n", file=sys.stderr)
            print(el2[max(0, idx - 160):idx + 260], file=sys.stderr)
        else:
            print("no __el2_stack symbol found", file=sys.stderr)
        sys.exit(1)

    el2_p.write_text(el2_new)
    print("patched EL2 stack storage")

PY

cmake --build build

echo
echo "patched EL2 per-CPU stacks"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

#!/usr/bin/env bash
set -euo pipefail

ASM="kernel/arch/arm64/syscall.S"
SCHED="kernel/sched/runqueue.cc"
GDB="scripts/debug/anko.gdb"

cp "$ASM" "$ASM.bak.remove-probes.$(date +%Y%m%d-%H%M%S)"
cp "$SCHED" "$SCHED.bak.addrspace-fix.$(date +%Y%m%d-%H%M%S)"
[[ -f "$GDB" ]] && cp "$GDB" "$GDB.bak.simple.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path
import re

# 1. Remove the return probes from syscall.S.
p = Path("kernel/arch/arm64/syscall.S")
s = p.read_text()

# Remove probe labels + mrs added earlier.
s = re.sub(
    r"\n\.global syscall_return_probe\n"
    r"\.type syscall_return_probe, %function\n"
    r"syscall_return_probe:\n"
    r"\s*mrs\s+x12,\s*TTBR0_EL1\n",
    "\n",
    s,
)

s = re.sub(
    r"\n\.global irq_return_probe\n"
    r"\.type irq_return_probe, %function\n"
    r"irq_return_probe:\n"
    r"\s*mrs\s+x12,\s*TTBR0_EL1\n",
    "\n",
    s,
)

# Remove older eret labels if they were added too. They are harmless, but clean them.
s = re.sub(
    r"\n\.global syscall_return_eret\n"
    r"(?:\.type syscall_return_eret, %function\n)?"
    r"syscall_return_eret:\n",
    "\n",
    s,
)

s = re.sub(
    r"\n\.global irq_return_eret\n"
    r"(?:\.type irq_return_eret, %function\n)?"
    r"irq_return_eret:\n",
    "\n",
    s,
)

p.write_text(s)

# 2. Patch post_syscall's no-switch-needed branch.
p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

old = """    if (!requeue && cur->state == TS_RUNNING) {
        // No switch needed: thread continues
        cur->last_start_ns = sched_clock_ns();
        return;
    }
"""

new = """    if (!requeue && cur->state == TS_RUNNING) {
        // No switch needed: thread continues, but the syscall path may have
        // temporarily activated another address space. Restore this thread's
        // user TTBR0 before returning to EL0.
        cur->last_start_ns = sched_clock_ns();
        activate_thread_space(cur);
        return;
    }
"""

if old not in s:
    raise SystemExit("could not find the post_syscall no-switch branch to patch")

s = s.replace(old, new, 1)
p.write_text(s)
PY

cat > "$GDB" <<'GDBEOF'
set pagination off
set confirm off
set breakpoint pending on
set architecture aarch64

file build/kernel/anko

add-symbol-file build/userland/init             0x10000000
add-symbol-file build/userland/nameservice      0x10100000
add-symbol-file build/userland/io.console       0x10200000
add-symbol-file build/userland/demo.pong        0x10300000
add-symbol-file build/userland/demo.ping        0x10400000
add-symbol-file build/userland/dango.manager    0x10500000
add-symbol-file build/userland/dango.auth_probe 0x10600000

define dump_fault
  set $tf = (unsigned long *)$x2
  set $ec = ($x0 >> 26) & 0x3f
  set $iss = $x0 & 0x01ffffff
  set $fsc = $x0 & 0x3f

  printf "\n--- fault dump ---\n"
  printf "EC  = 0x%lx\n", $ec
  printf "ISS = 0x%lx\n", $iss
  printf "FSC = 0x%lx\n", $fsc
  printf "ESR = 0x%lx\n", $x0
  printf "FAR = 0x%lx\n", $x1
  printf "TF  = 0x%lx\n", $x2

  printf "saved ELR  = 0x%lx\n", $tf[31]
  printf "saved SPSR = 0x%lx\n", $tf[32]
  printf "saved SP   = 0x%lx\n", $tf[33]

  printf "\nSymbol at saved ELR:\n"
  p/a $tf[31]
  info symbol $tf[31]

  printf "\nSaved registers:\n"
  printf "x0  = 0x%lx\n", $tf[0]
  printf "x1  = 0x%lx\n", $tf[1]
  printf "x2  = 0x%lx\n", $tf[2]
  printf "x3  = 0x%lx\n", $tf[3]
  printf "x4  = 0x%lx\n", $tf[4]
  printf "x5  = 0x%lx\n", $tf[5]
  printf "x6  = 0x%lx\n", $tf[6]
  printf "x7  = 0x%lx\n", $tf[7]
  printf "x8  = 0x%lx\n", $tf[8]
  printf "x29 = 0x%lx\n", $tf[29]
  printf "x30 = 0x%lx\n", $tf[30]

  printf "\nKernel backtrace:\n"
  bt
end

target remote :1234

break syscall_fault
commands
  silent
  set $ec = ($x0 >> 26) & 0x3f

  if $ec == 0x3c
    set $tf = (unsigned long *)$x2
    printf "\n--- ignoring EL0 BRK/trap at "
    p/a $tf[31]
    continue
  end

  printf "\n--- EL0 real fault ---\n"
  dump_fault
  printf "\nStopped.\n"
end

break kernel_sync_fault
commands
  silent
  printf "\n--- EL1 kernel fault ---\n"
  dump_fault
  printf "\nStopped.\n"
end

rbreak ^panic

continue
GDBEOF

cmake --build build

echo
echo "Done. Now run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

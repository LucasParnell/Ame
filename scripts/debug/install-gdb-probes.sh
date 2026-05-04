#!/usr/bin/env bash
set -euo pipefail

ASM="kernel/arch/arm64/syscall.S"
GDB="scripts/debug/anko.gdb"

if [[ ! -f "$ASM" ]]; then
  echo "error: $ASM not found" >&2
  exit 1
fi

mkdir -p scripts/debug

cp "$ASM" "$ASM.bak.$(date +%Y%m%d-%H%M%S)"
[[ -f "$GDB" ]] && cp "$GDB" "$GDB.bak.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path
import re

p = Path("kernel/arch/arm64/syscall.S")
s = p.read_text()

# Remove old probe blocks if this script is rerun.
for name in ("syscall_return_probe", "irq_return_probe"):
    s = re.sub(
        rf"\n\.global {name}\n\.type {name}, %function\n{name}:\n\s*mrs\s+x12,\s*TTBR0_EL1\n",
        "\n",
        s,
    )

needle = re.compile(
    r"(    msr\s+SP_EL0,\s+x11\n)"
    r"(\n    // Restore ALL GP registers)"
)

matches = list(needle.finditer(s))
if len(matches) < 2:
    raise SystemExit(
        f"expected at least 2 EL0 return epilogues in syscall.S, found {len(matches)}"
    )

out = []
last = 0
for i, m in enumerate(matches[:2]):
    label = "syscall_return_probe" if i == 0 else "irq_return_probe"
    out.append(s[last:m.end(1)])
    out.append(
        f"\n.global {label}\n"
        f".type {label}, %function\n"
        f"{label}:\n"
        f"    mrs     x12, TTBR0_EL1\n"
    )
    out.append(s[m.end(1):m.end(2)])
    last = m.end(2)

out.append(s[last:])
s = "".join(out)
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

# Probe point is before GP restore:
#   x9  = ELR_EL1 value that will be returned to
#   x10 = SPSR_EL1 value that will be returned with
#   x11 = SP_EL0 value that will be returned with
#   x12 = TTBR0_EL1 read by our probe
break syscall_return_probe
commands
  silent
  if $x9 >= 0x10000000 && $x9 < 0x10700000
    printf "\n--- syscall return probe ---\n"
    printf "return ELR = 0x%lx\n", $x9
    printf "return SPSR = 0x%lx\n", $x10
    printf "return SP   = 0x%lx\n", $x11
    printf "TTBR0_EL1   = 0x%lx\n", $x12
    info symbol $x9
  end
  continue
end

break irq_return_probe
commands
  silent
  if $x9 >= 0x10000000 && $x9 < 0x10700000
    printf "\n--- irq return probe ---\n"
    printf "return ELR = 0x%lx\n", $x9
    printf "return SPSR = 0x%lx\n", $x10
    printf "return SP   = 0x%lx\n", $x11
    printf "TTBR0_EL1   = 0x%lx\n", $x12
    info symbol $x9
  end
  continue
end

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
  printf "\nStopped. Last return probe above should show the TTBR0 used before eret.\n"
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

echo "patched $ASM"
echo "rewrote $GDB"
echo
echo "rebuilding..."
cmake --build build

echo
echo "done. now run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

#!/usr/bin/env bash
set -euo pipefail

ASM="kernel/arch/arm64/syscall.S"
GDB="scripts/debug/anko.gdb"
KERNEL="build/kernel/anko"

if [[ ! -f "$ASM" ]]; then
  echo "error: $ASM not found" >&2
  exit 1
fi

mkdir -p scripts/debug

cp "$ASM" "$ASM.bak.$(date +%Y%m%d-%H%M%S)"
[[ -f "$GDB" ]] && cp "$GDB" "$GDB.bak.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("kernel/arch/arm64/syscall.S")
s = p.read_text()

# Remove stale duplicate labels if this script was run before.
for label in [
    ".global syscall_return_eret\n.type syscall_return_eret, %function\nsyscall_return_eret:\n",
    ".global irq_return_eret\n.type irq_return_eret, %function\nirq_return_eret:\n",
    ".global syscall_return_eret\nsyscall_return_eret:\n",
    ".global irq_return_eret\nirq_return_eret:\n",
]:
    s = s.replace(label, "")

marker = "    add     sp, sp, #288\n    eret\n"
idx1 = s.find(marker)
if idx1 < 0:
    raise SystemExit("could not find syscall EL0 eret pattern")

s = (
    s[:idx1]
    + "    add     sp, sp, #288\n"
      ".global syscall_return_eret\n"
      ".type syscall_return_eret, %function\n"
      "syscall_return_eret:\n"
      "    eret\n"
    + s[idx1 + len(marker):]
)

idx2 = s.find(marker, idx1 + 1)
if idx2 < 0:
    raise SystemExit("could not find irq EL0 eret pattern")

s = (
    s[:idx2]
    + "    add     sp, sp, #288\n"
      ".global irq_return_eret\n"
      ".type irq_return_eret, %function\n"
      "irq_return_eret:\n"
      "    eret\n"
    + s[idx2 + len(marker):]
)

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

break syscall_return_eret
commands
  silent
  set $ret_elr = (unsigned long)$elr_el1

  if $ret_elr >= 0x10000000 && $ret_elr < 0x10700000
    printf "\n--- about to syscall eret to EL0 ---\n"
    printf "ELR_EL1   = 0x%lx\n", $elr_el1
    printf "SPSR_EL1  = 0x%lx\n", $spsr_el1
    printf "SP_EL0    = 0x%lx\n", $sp_el0
    printf "TTBR0_EL1 = 0x%lx\n", $ttbr0_el1
    info symbol $elr_el1
    continue
  else
    continue
  end
end

break irq_return_eret
commands
  silent
  set $ret_elr = (unsigned long)$elr_el1

  if $ret_elr >= 0x10000000 && $ret_elr < 0x10700000
    printf "\n--- about to irq eret to EL0 ---\n"
    printf "ELR_EL1   = 0x%lx\n", $elr_el1
    printf "SPSR_EL1  = 0x%lx\n", $spsr_el1
    printf "SP_EL0    = 0x%lx\n", $sp_el0
    printf "TTBR0_EL1 = 0x%lx\n", $ttbr0_el1
    info symbol $elr_el1
    continue
  else
    continue
  end
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
  printf "\nStopped. Use 'continue' to resume.\n"
end

break kernel_sync_fault
commands
  silent
  printf "\n--- EL1 kernel fault ---\n"
  dump_fault
  printf "\nStopped. Use 'continue' to resume.\n"
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
echo "checking for eret labels..."
if ! aarch64-linux-gnu-nm "$KERNEL" | grep -E 'syscall_return_eret|irq_return_eret'; then
  echo "error: eret labels are still missing from $KERNEL" >&2
  exit 1
fi

echo
echo "ok. now run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

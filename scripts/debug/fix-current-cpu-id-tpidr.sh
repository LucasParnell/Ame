#!/usr/bin/env bash
set -euo pipefail

RUNQ="kernel/sched/runqueue.cc"
MAIN="kernel/kernel_main.cc"
SMP="kernel/smp.cc"
EL2="kernel/arch/arm64/el2.S"

cp "$RUNQ" "$RUNQ.bak.cpu-id.$(date +%Y%m%d-%H%M%S)"
cp "$MAIN" "$MAIN.bak.cpu-id.$(date +%Y%m%d-%H%M%S)"
cp "$SMP"  "$SMP.bak.cpu-id.$(date +%Y%m%d-%H%M%S)"
cp "$EL2"  "$EL2.bak.vmpidr-restore.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

# 1. Make sched::current_cpu_id() use TPIDR_EL1, not MPIDR_EL1.
p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

old = '''u32 current_cpu_id() {
    u64 mpidr;
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return static_cast<u32>(mpidr & 0xFFu);
}
'''

new = '''u32 current_cpu_id() {
    // Do not use MPIDR_EL1 here.  Once EL2 virtualization is active,
    // VMPIDR_EL2 can affect what EL1 observes as MPIDR_EL1.  The host
    // scheduler needs a stable physical/logical CPU id, so we keep it in
    // TPIDR_EL1 on each host CPU.
    u64 cpu_id = 0;
    asm volatile("mrs %0, TPIDR_EL1" : "=r"(cpu_id));
    if (cpu_id >= kMaxCpus) return 0;
    return static_cast<u32>(cpu_id);
}
'''

if old not in s:
    raise SystemExit("could not find current_cpu_id() block")
s = s.replace(old, new, 1)
p.write_text(s)

# 2. Set TPIDR_EL1 = 0 on the boot CPU.
p = Path("kernel/kernel_main.cc")
s = p.read_text()

needle = '''extern "C" [[noreturn]] void kernel_main() {
'''
insert = '''extern "C" [[noreturn]] void kernel_main() {
    {
        const u64 boot_cpu_id = 0;
        asm volatile("msr TPIDR_EL1, %0\\n"
                     "isb\\n"
                     :
                     : "r"(boot_cpu_id)
                     : "memory");
    }
'''

if "boot_cpu_id = 0" not in s:
    if needle not in s:
        raise SystemExit("could not find kernel_main entry")
    s = s.replace(needle, insert, 1)
p.write_text(s)

# 3. Set TPIDR_EL1 on secondary CPUs before they enter the scheduler.
p = Path("kernel/smp.cc")
s = p.read_text()

needle = '''extern "C" [[noreturn]] void secondary_kernel_main(u64 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
'''
insert = '''extern "C" [[noreturn]] void secondary_kernel_main(u64 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
    asm volatile("msr TPIDR_EL1, %0\\n"
                 "isb\\n"
                 :
                 : "r"(cpu_id)
                 : "memory");
'''

if "msr TPIDR_EL1" not in s[s.find("secondary_kernel_main"):s.find("secondary_kernel_main")+400]:
    if needle not in s:
        raise SystemExit("could not find secondary_kernel_main entry")
    s = s.replace(needle, insert, 1)
p.write_text(s)

# 4. Hygiene: restore host-looking VMPIDR_EL2/VPIDR_EL2 on VM exit.
#    This is not the main scheduler fix anymore, but it prevents host EL1
#    MPIDR reads from seeing stale guest virtual CPU identity.
p = Path("kernel/arch/arm64/el2.S")
s = p.read_text()

needle = '''    mov     x0, #0x80000000
    msr     HCR_EL2, x0
    msr     VTTBR_EL2, xzr
'''

insert = '''    mov     x0, #0x80000000
    msr     HCR_EL2, x0

    // Return host-visible virtual CPU ID registers to physical-looking values.
    // Host scheduler code should not depend on MPIDR_EL1 anymore, but keeping
    // VMPIDR_EL2 sane avoids future surprises.
    mrs     x1, MPIDR_EL1
    msr     VMPIDR_EL2, x1
    mrs     x1, MIDR_EL1
    msr     VPIDR_EL2, x1

    msr     VTTBR_EL2, xzr
'''

if "Return host-visible virtual CPU ID registers" not in s:
    if needle not in s:
        raise SystemExit("could not find EL2 host-exit HCR/VTTBR block")
    s = s.replace(needle, insert, 1)
p.write_text(s)
PY

cmake --build build

echo
echo "patched current_cpu_id() to use TPIDR_EL1 and restored VMPIDR_EL2 on VM exit"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

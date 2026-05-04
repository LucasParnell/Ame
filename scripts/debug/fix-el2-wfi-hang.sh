#!/usr/bin/env bash
set -euo pipefail

EL2="kernel/arch/arm64/el2.S"
VCPU="kernel/vm/vcpu.cc"
MANAGER="userland/dango/manager/main.cc"

cp "$EL2" "$EL2.bak.wfi-hang.$(date +%Y%m%d-%H%M%S)"
cp "$VCPU" "$VCPU.bak.wfi-hang.$(date +%Y%m%d-%H%M%S)"
cp "$MANAGER" "$MANAGER.bak.wfi-hang.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

# 1. EL2: do not trap guest WFI/WFE for now, and do not TLBI on every exit.
p = Path("kernel/arch/arm64/el2.S")
s = p.read_text()

s = s.replace(
'''    orr     x0, x0, #(1 << 13)   // TWI: trap guest WFI so the VMM can schedule/tick.
    orr     x0, x0, #(1 << 14)   // TWE: trap guest WFE for the same reason.
''',
'''    // Do not trap guest WFI/WFE here.  With IMO/FMO/AMO set, host physical
    // IRQs still route to EL2 and wake the guest run.  Trapping guest idle
    // creates a tight WFx-exit loop once Linux reaches idle.
'''
)

s = s.replace(
'''    msr     VTTBR_EL2, xzr
    dsb     ish
    tlbi    vmalls12e1
    dsb     ish
    isb
''',
'''    msr     VTTBR_EL2, xzr
    // Do not invalidate all stage-1+stage-2 translations on every guest exit.
    // The exit path runs for idle/timer exits too; doing VMALLS12E1 here can
    // wedge in the following barrier and is far too expensive.  Stage-2 TLB
    // invalidation belongs on stage-2 map/unmap or before guest entry when the
    // stage-2 root/generation changes.
    isb
''',
1
)

p.write_text(s)

# 2. Kernel VCPU loop: WFx is not guest progress. Return to userspace/runner.
p = Path("kernel/vm/vcpu.cc")
s = p.read_text()

old = '''        if (ec == kEcWfx) {
            /*
             * WFI/WFE are normal guest idle instructions.  We intentionally
             * trap them instead of allowing the physical CPU to sleep because
             * Anko must keep ownership of virtual timer/IRQ scheduling.
             * Complete the instruction, account for it, and continue without
             * printing per-idle-cycle diagnostics.
             */
            vcpu->el2.pc += 4;
            metric_inc(VM_DEBUG_VCPU_WFX_EXITS);
            continue;
        }
'''

new = '''        if (ec == kEcWfx) {
            /*
             * WFI/WFE are normal guest idle instructions.  Do not spin inside
             * one sys_vcpu_run() consuming the entire syscall budget on idle
             * exits.  Complete the trapped instruction and return to the async
             * runner so the host scheduler can run other work.
             */
            vcpu->el2.pc += 4;
            sync_vcpu_from_el2(*vcpu);
            vcpu->state = VCPU_READY;
            owner->state = VM_READY;
            vcpu->last_exit_reason = esr;
            metric_inc(VM_DEBUG_VCPU_WFX_EXITS);
            return E_OK;
        }
'''

if old not in s:
    raise SystemExit("could not find existing WFx handler in kernel/vm/vcpu.cc")

s = s.replace(old, new, 1)
p.write_text(s)

# 3. User runner: yield after each run, otherwise the async runner immediately
# re-enters the kernel forever once the guest is idle.
p = Path("userland/dango/manager/main.cc")
s = p.read_text()

old = '''static void runner_entry(uint64_t vcpu_arg) {
    const CapHandle vcpu = static_cast<CapHandle>(vcpu_arg);
    while (true) {
        (void)sys_vcpu_run(vcpu, kLinuxRunBudgetTicks);
    }
}
'''

new = '''static void runner_entry(uint64_t vcpu_arg) {
    const CapHandle vcpu = static_cast<CapHandle>(vcpu_arg);
    while (true) {
        (void)sys_vcpu_run(vcpu, kLinuxRunBudgetTicks);
        sys_yield();
    }
}
'''

if old not in s:
    raise SystemExit("could not find runner_entry in userland/dango/manager/main.cc")

s = s.replace(old, new, 1)
p.write_text(s)
PY

cmake --build build

echo
echo "patched EL2 WFI/WFE idle-exit hang"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

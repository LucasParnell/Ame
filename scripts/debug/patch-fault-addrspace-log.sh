#!/usr/bin/env bash
set -euo pipefail

FILE="kernel/syscall/dispatch.cc"
cp "$FILE" "$FILE.bak.fault-addrspace.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("kernel/syscall/dispatch.cc")
s = p.read_text()

helper = r'''
static u64 read_ttbr0_el1_debug() {
    u64 v = 0;
    asm volatile("mrs %0, TTBR0_EL1" : "=r"(v) :: "memory");
    return v;
}

static void log_fault_addrspace_debug(Process* p, Thread* t, TrapFrame* tf, u64 far) {
    const u64 active_ttbr0 = read_ttbr0_el1_debug();
    const u64 proc_ttbr0 = p ? p->ttbr0 : 0;

    VmSpace space{};
    space.pgd_phys = proc_ttbr0;
    space.pid = p ? p->pid : 0;

    const u64 elr = tf ? tf->elr : 0;
    const u64 elr_pte = proc_ttbr0 ? mm::vm_debug_pte(space, elr) : 0;
    const u64 far_pte = proc_ttbr0 ? mm::vm_debug_pte(space, far) : 0;

    ANKO_LOGF_FATAL("kernel/fault",
                    "addrspace-debug active_ttbr0=%llx proc_ttbr0=%llx "
                    "pid=%u tid=%u cur_pid=%u elr=%llx far=%llx "
                    "elr_pte=%llx far_pte=%llx",
                    static_cast<unsigned long long>(active_ttbr0),
                    static_cast<unsigned long long>(proc_ttbr0),
                    p ? p->pid : 0,
                    t ? t->tid : 0,
                    t ? t->pid : 0,
                    static_cast<unsigned long long>(elr),
                    static_cast<unsigned long long>(far),
                    static_cast<unsigned long long>(elr_pte),
                    static_cast<unsigned long long>(far_pte));
}
'''

anchor = "void record_supervisor_event(Process* proc, long status) {"
if "read_ttbr0_el1_debug" not in s:
    s = s.replace(anchor, helper + "\n" + anchor, 1)

needle = '''    } else {
        ANKO_LOGF_FATAL("kernel/fault",
'''

if "log_fault_addrspace_debug(p, ct, tf, far);" not in s:
    s = s.replace(
        needle,
        '''    } else {
        log_fault_addrspace_debug(p, ct, tf, far);
        ANKO_LOGF_FATAL("kernel/fault",
''',
        1,
    )

p.write_text(s)
PY

cmake --build build

echo
echo "patched syscall_fault addrspace logging"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

#!/usr/bin/env bash
set -euo pipefail

LOG="kernel/log/log.cc"
SCHED="kernel/sched/runqueue.cc"

cp "$LOG" "$LOG.bak.log-wedge.$(date +%Y%m%d-%H%M%S)"
cp "$SCHED" "$SCHED.bak.log-wedge.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

# 1. Logger: use TPIDR_EL1 for CPU index, not MPIDR_EL1.
p = Path("kernel/log/log.cc")
s = p.read_text()

old_cpu = '''u32 cpu_index() {
    u64 mpidr = 0;
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    const u32 id = static_cast<u32>(mpidr & 0xFFu);
    return id < kMaxCpus ? id : 0;
}
'''

new_cpu = '''u32 cpu_index() {
    // Match sched::current_cpu_id().  MPIDR_EL1 is not a reliable host CPU id
    // once EL2/VMPIDR virtualization is active.
    u64 id = 0;
    asm volatile("mrs %0, TPIDR_EL1" : "=r"(id));
    return id < kMaxCpus ? static_cast<u32>(id) : 0;
}
'''

if old_cpu in s:
    s = s.replace(old_cpu, new_cpu, 1)

# 2. Logger: do not emit drop notices from the hot log path.  The old path
# built and pushed another log record while already inside emit_line(), which
# becomes toxic under scheduler switch-log floods.
old_call = '''    if (console_attached_for(cpu)) {
        emit_drop_notice(cpu);
        if (!push_bytes(g_rings[cpu], line, len)) {
            __atomic_add_fetch(&g_dropped[cpu], 1ull, __ATOMIC_RELAXED);
        }
    }
'''

new_call = '''    if (console_attached_for(cpu)) {
        if (!push_bytes(g_rings[cpu], line, len)) {
            __atomic_add_fetch(&g_dropped[cpu], 1ull, __ATOMIC_RELAXED);
        }
    }
'''

if old_call not in s:
    raise SystemExit("could not find emit_drop_notice call in emit_line")
s = s.replace(old_call, new_call, 1)

p.write_text(s)

# 3. Scheduler: remove switch logging from post_syscall hot path.
p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

old_log = '''    ANKO_LOGF_INFO("kernel/sched",
                   "switch tid=%u pid=%u elr=%llx state=%u",
                   next->tid, next->pid,
                   static_cast<unsigned long long>(next->tf.elr),
                   static_cast<unsigned>(next->state));
'''

new_log = '''    // Do not log every context switch.  This path runs on every yield/syscall
    // and can overwhelm the kernel log ring badly enough to wedge inside the
    // logger/drop-notice path.
#if 0
    ANKO_LOGF_INFO("kernel/sched",
                   "switch tid=%u pid=%u elr=%llx state=%u",
                   next->tid, next->pid,
                   static_cast<unsigned long long>(next->tf.elr),
                   static_cast<unsigned>(next->state));
#endif
'''

if old_log not in s:
    raise SystemExit("could not find scheduler switch log block")
s = s.replace(old_log, new_log, 1)

p.write_text(s)
PY

cmake --build build

echo
echo "patched logger wedge + disabled scheduler switch spam"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

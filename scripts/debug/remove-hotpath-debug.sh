#!/usr/bin/env bash
set -euo pipefail

RUNQ="kernel/sched/runqueue.cc"
DISPATCH="kernel/syscall/dispatch.cc"

cp "$RUNQ" "$RUNQ.bak.remove-hotpath-debug.$(date +%Y%m%d-%H%M%S)"
cp "$DISPATCH" "$DISPATCH.bak.remove-hotpath-debug.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path
import re

# ---------------------------------------------------------------------
# 1. Remove frame-owner page-table scans from post_syscall hot path.
# ---------------------------------------------------------------------
p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

s = re.sub(
    r'\nstatic bool process_maps_elr\(Process\* p, u64 elr\) \{.*?'
    r'\nstatic void debug_check_frame_owner\(const char\* where, Thread\* cur, TrapFrame\* frame\) \{.*?\n\}\n',
    '\n',
    s,
    flags=re.S,
)

s = s.replace(
    '    debug_check_frame_owner("before_save_cur_tf", cur, frame);\n',
    '',
)

s = s.replace(
    '    debug_check_frame_owner("before_restore_next_tf", next, &next->tf);\n',
    '',
)

# Disable switch spam too, if it is still present.
switch_log = '''    ANKO_LOGF_INFO("kernel/sched",
                   "switch tid=%u pid=%u elr=%llx state=%u",
                   next->tid, next->pid,
                   static_cast<unsigned long long>(next->tf.elr),
                   static_cast<unsigned>(next->state));
'''
s = s.replace(switch_log, '')

p.write_text(s)

# ---------------------------------------------------------------------
# 2. Remove addrspace-debug page-table scans from syscall_fault.
# ---------------------------------------------------------------------
p = Path("kernel/syscall/dispatch.cc")
s = p.read_text()

s = re.sub(
    r'\nstatic u64 read_ttbr0_el1_debug\(\) \{.*?'
    r'\nstatic void log_fault_addrspace_debug\(Process\* p, Thread\* t, TrapFrame\* tf, u64 far\) \{.*?\n\}\n',
    '\n',
    s,
    flags=re.S,
)

s = s.replace(
    '        log_fault_addrspace_debug(p, ct, tf, far);\n',
    '',
)

p.write_text(s)
PY

cmake --build build

echo
echo "removed syscall/scheduler hot-path debug page-table walks"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

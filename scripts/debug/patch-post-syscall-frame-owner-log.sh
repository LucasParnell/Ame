#!/usr/bin/env bash
set -euo pipefail

FILE="kernel/sched/runqueue.cc"
cp "$FILE" "$FILE.bak.frame-owner.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

helper = r'''
static bool process_maps_elr(Process* p, u64 elr) {
    if (!p || !p->ttbr0) return false;
    VmSpace space{};
    space.pgd_phys = p->ttbr0;
    space.pid = p->pid;
    const u64 pte = mm::vm_debug_pte(space, elr);
    return pte != 0;
}

static Process* find_process_mapping_elr(u64 elr) {
    for (u32 pid = 1; pid <= 64; ++pid) {
        Process* p = find_process(pid);
        if (process_maps_elr(p, elr)) return p;
    }
    return nullptr;
}

static void debug_check_frame_owner(const char* where, Thread* cur, TrapFrame* frame) {
    if (!cur || !frame) return;

    Process* curp = find_process(cur->pid);
    if (process_maps_elr(curp, frame->elr)) return;

    Process* owner = find_process_mapping_elr(frame->elr);

    ANKO_LOGF_FATAL("kernel/sched",
                    "FRAME_OWNER_MISMATCH where=%s "
                    "current_tid=%u current_pid=%u current_proc=%s "
                    "frame_elr=%llx frame_sp=%llx "
                    "mapped_by_pid=%u mapped_by_proc=%s "
                    "cur_tf_elr=%llx cur_tf_sp=%llx",
                    where,
                    cur ? cur->tid : 0,
                    cur ? cur->pid : 0,
                    (curp && curp->name[0]) ? curp->name : "?",
                    static_cast<unsigned long long>(frame ? frame->elr : 0),
                    static_cast<unsigned long long>(frame ? frame->sp_el0 : 0),
                    owner ? owner->pid : 0,
                    (owner && owner->name[0]) ? owner->name : "?",
                    static_cast<unsigned long long>(cur ? cur->tf.elr : 0),
                    static_cast<unsigned long long>(cur ? cur->tf.sp_el0 : 0));
}
'''

anchor = "// ---------------------------------------------------------------------------\n// post_syscall"
if "debug_check_frame_owner" not in s:
    s = s.replace(anchor, helper + "\n" + anchor, 1)

old = '''    auto* frame = static_cast<TrapFrame*>(frame_ptr);
    cur->tf = *frame;
'''

new = '''    auto* frame = static_cast<TrapFrame*>(frame_ptr);
    debug_check_frame_owner("before_save_cur_tf", cur, frame);
    cur->tf = *frame;
'''

if old not in s:
    raise SystemExit("could not find cur->tf save block")

s = s.replace(old, new, 1)

old2 = '''    activate_thread_space(next);
    *frame = next->tf;
'''

new2 = '''    activate_thread_space(next);
    debug_check_frame_owner("before_restore_next_tf", next, &next->tf);
    *frame = next->tf;
'''

if old2 in s:
    s = s.replace(old2, new2, 1)
else:
    print("warning: could not patch restore-next check")

p.write_text(s)
PY

cmake --build build

echo
echo "patched post_syscall frame-owner logging"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

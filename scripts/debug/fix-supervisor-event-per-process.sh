#!/usr/bin/env bash
set -euo pipefail

SCHED_H="kernel/include/anko/sched.h"
DISPATCH="kernel/syscall/dispatch.cc"

cp "$SCHED_H" "$SCHED_H.bak.supervisor-per-process.$(date +%Y%m%d-%H%M%S)"
cp "$DISPATCH" "$DISPATCH.bak.supervisor-per-process.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path
import re

# ---------------------------------------------------------------------
# Add per-supervisor event slot to Process.
# ---------------------------------------------------------------------
p = Path("kernel/include/anko/sched.h")
s = p.read_text()

if "supervisor_event_pending" not in s:
    old = '''    u64         runtime_ns;
    u32         runnable_threads;
    u32         _pad1;
};
'''

    new = '''    u64         runtime_ns;
    u32         runnable_threads;

    // Per-supervisor child-death event slot.  Written by a dying child on
    // one CPU and polled by the supervisor process on another.
    u32         supervisor_event_lock;
    u32         supervisor_event_pending;
    u32         supervisor_event_pid;
    u32         _pad1;
    long        supervisor_event_status;
};
'''

    if old not in s:
        raise SystemExit("could not find Process tail in sched.h")

    s = s.replace(old, new, 1)
    p.write_text(s)

# ---------------------------------------------------------------------
# Replace global supervisor event slot with per-supervisor Process slot.
# ---------------------------------------------------------------------
p = Path("kernel/syscall/dispatch.cc")
s = p.read_text()

# Remove old global event slot, if present.
s = re.sub(
    r'\nbool g_supervisor_event_pending = false;\n'
    r'u32 g_supervisor_event_pid = 0;\n'
    r'u32 g_supervisor_event_supervisor = 0;\n'
    r'long g_supervisor_event_status = 0;\n',
    '\n',
    s,
)

# Remove earlier attempted global-lock slot, if present.
s = re.sub(
    r'\n// Supervisor death event slot\..*?'
    r'u32 g_supervisor_event_lock = 0;.*?'
    r'long g_supervisor_event_status = 0;\n',
    '\n',
    s,
    flags=re.S,
)

helpers_and_record = r'''
static void supervisor_event_lock(Process* p) {
    if (!p) return;
    while (__atomic_exchange_n(&p->supervisor_event_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        asm volatile("yield" ::: "memory");
    }
}

static void supervisor_event_unlock(Process* p) {
    if (!p) return;
    __atomic_store_n(&p->supervisor_event_lock, 0u, __ATOMIC_RELEASE);
}

static void wake_supervisor_process(Process* supervisor) {
    if (!supervisor) return;

    for (Thread* t = supervisor->threads; t; t = t->process_next) {
        if (t->state != TS_DEAD) {
            sched::wake(t);
        }
    }

    // Wake CPUs that may be sleeping in WFI.
    asm volatile("sev" ::: "memory");
}

void record_supervisor_event(Process* proc, long status) {
    if (!proc || proc->supervisor_pid == 0) {
        ANKO_LOGF_WARN("kernel/sched",
                       "supervisor-event drop no-supervisor dead_pid=%u status=%llx",
                       proc ? proc->pid : 0,
                       static_cast<unsigned long long>(status));
        return;
    }

    Process* supervisor = find_process(proc->supervisor_pid);
    if (!supervisor) {
        ANKO_LOGF_WARN("kernel/sched",
                       "supervisor-event drop missing-supervisor supervisor_pid=%u dead_pid=%u status=%llx",
                       proc->supervisor_pid,
                       proc->pid,
                       static_cast<unsigned long long>(status));
        return;
    }

    supervisor_event_lock(supervisor);

    if (__atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_RELAXED) == 0) {
        supervisor->supervisor_event_pid = proc->pid;
        supervisor->supervisor_event_status = status;
        __atomic_store_n(&supervisor->supervisor_event_pending, 1u, __ATOMIC_RELEASE);

        ANKO_LOGF_WARN("kernel/sched",
                       "supervisor-event publish supervisor_pid=%u dead_pid=%u status=%llx",
                       supervisor->pid,
                       proc->pid,
                       static_cast<unsigned long long>(status));
    } else {
        ANKO_LOGF_WARN("kernel/sched",
                       "supervisor-event already-pending supervisor_pid=%u existing_dead_pid=%u new_dead_pid=%u",
                       supervisor->pid,
                       supervisor->supervisor_event_pid,
                       proc->pid);
    }

    supervisor_event_unlock(supervisor);
    wake_supervisor_process(supervisor);
}
'''

# Replace any existing helper+record block or the original record function.
s, n = re.subn(
    r'\n(?:static void supervisor_event_lock\(\).*?\n)?'
    r'void record_supervisor_event\(Process\* proc, long status\) \{.*?\n\}',
    '\n' + helpers_and_record,
    s,
    count=1,
    flags=re.S,
)

if n != 1:
    raise SystemExit("could not replace record_supervisor_event()")

new_poll = r'''    case SYS_SUPERVISOR_POLL: {
        CapEntry* supervisor_e =
            cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_SUPERVISOR);
        if (!supervisor_e) return err(E_NOTFOUND);
        if (!has_rights(supervisor_e, RIGHT_READ)) return err(E_PERM);
        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);

        if (__atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_ACQUIRE) == 0) {
            return err(E_NOTFOUND);
        }

        supervisor_event_lock(supervisor);

        if (__atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_ACQUIRE) == 0) {
            supervisor_event_unlock(supervisor);
            return err(E_NOTFOUND);
        }

        const u32 dead_pid = supervisor->supervisor_event_pid;
        const long status = supervisor->supervisor_event_status;

        supervisor->supervisor_event_pid = 0;
        supervisor->supervisor_event_status = 0;
        __atomic_store_n(&supervisor->supervisor_event_pending, 0u, __ATOMIC_RELEASE);

        supervisor_event_unlock(supervisor);

        ANKO_LOGF_WARN("kernel/sched",
                       "supervisor-event poll-hit supervisor_pid=%u dead_pid=%u status=%llx",
                       supervisor->pid,
                       dead_pid,
                       static_cast<unsigned long long>(status));

        return ok(static_cast<long>(dead_pid), status);
    }

'''

s, n = re.subn(
    r'    case SYS_SUPERVISOR_POLL: \{.*?\n    \}\n\n    case SYS_THREAD_CREATE:',
    new_poll + '    case SYS_THREAD_CREATE:',
    s,
    count=1,
    flags=re.S,
)

if n != 1:
    raise SystemExit("could not replace SYS_SUPERVISOR_POLL block")

p.write_text(s)
PY

cmake --build build

echo
echo "patched supervisor events to be per-supervisor-process"
echo "run:"
echo "  ./scripts/run-qemu.sh debug"
echo "  aarch64-linux-gnu-gdb -x scripts/debug/anko.gdb"

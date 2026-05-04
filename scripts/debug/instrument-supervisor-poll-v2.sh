#!/usr/bin/env bash
set -euo pipefail

FILE="kernel/syscall/dispatch.cc"
cp "$FILE" "$FILE.bak.supervisor-poll-debug.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("kernel/syscall/dispatch.cc")
s = p.read_text()

# Add a debug counter inside the anonymous namespace.
if "g_supervisor_poll_debug_count" not in s:
    if "u64 g_el0_fault_count = 0;" in s:
        s = s.replace(
            "u64 g_el0_fault_count = 0;",
            "u64 g_el0_fault_count = 0;\nu64 g_supervisor_poll_debug_count = 0;",
            1,
        )
    else:
        raise SystemExit("could not find syscall globals to add poll counter")

helper = r'''
static void debug_log_supervisor_threads(Process* supervisor, const char* where) {
    if (!supervisor) {
        ANKO_LOGF_WARN("kernel/sched", "supervisor-debug where=%s supervisor=null", where);
        return;
    }

    for (Thread* t = supervisor->threads; t; t = t->process_next) {
        ANKO_LOGF_WARN("kernel/sched",
                       "supervisor-debug where=%s supervisor_pid=%u tid=%u "
                       "state=%u cpu=%u on_rq=%u elr=%llx sp=%llx",
                       where,
                       supervisor->pid,
                       t->tid,
                       static_cast<unsigned>(t->state),
                       t->cpu_id,
                       t->on_runqueue ? 1u : 0u,
                       static_cast<unsigned long long>(t->tf.elr),
                       static_cast<unsigned long long>(t->tf.sp_el0));
    }
}
'''

if "debug_log_supervisor_threads" not in s:
    marker = "\nvoid record_supervisor_event(Process* proc, long status) {"
    if marker not in s:
        raise SystemExit("could not find record_supervisor_event() insertion point")
    s = s.replace(marker, "\n" + helper + marker, 1)

# Add supervisor thread-state logging around publish/wake if not already present.
if 'debug_log_supervisor_threads(supervisor, "after_publish_before_wake");' not in s:
    old = '''    supervisor_event_unlock(supervisor);
    wake_supervisor_process(supervisor);
'''
    new = '''    debug_log_supervisor_threads(supervisor, "after_publish_before_wake");
    supervisor_event_unlock(supervisor);
    wake_supervisor_process(supervisor);
    debug_log_supervisor_threads(supervisor, "after_publish_after_wake");
'''
    if old in s:
        s = s.replace(old, new, 1)
    else:
        print("warning: could not instrument publish/wake block")

# Replace the entire SYS_SUPERVISOR_POLL case by locating the next case label.
start = s.find("    case SYS_SUPERVISOR_POLL:")
if start == -1:
    raise SystemExit("could not find case SYS_SUPERVISOR_POLL")

end = s.find("    case SYS_THREAD_CREATE:", start)
if end == -1:
    raise SystemExit("could not find case SYS_THREAD_CREATE after SYS_SUPERVISOR_POLL")

replacement = r'''    case SYS_SUPERVISOR_POLL: {
        CapEntry* supervisor_e =
            cap::lookup(*p->cap_table, static_cast<CapHandle>(a0), CAP_SUPERVISOR);
        if (!supervisor_e) {
            ANKO_LOGF_WARN("kernel/sched",
                           "supervisor-poll no-cap cur_pid=%u cap=%llx",
                           p ? p->pid : 0,
                           static_cast<unsigned long long>(a0));
            return err(E_NOTFOUND);
        }
        if (!has_rights(supervisor_e, RIGHT_READ)) {
            ANKO_LOGF_WARN("kernel/sched",
                           "supervisor-poll no-right cur_pid=%u rights=%x",
                           p ? p->pid : 0,
                           supervisor_e->rights_mask);
            return err(E_PERM);
        }

        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) {
            ANKO_LOGF_WARN("kernel/sched",
                           "supervisor-poll null-target cur_pid=%u",
                           p ? p->pid : 0);
            return err(E_NOTFOUND);
        }

        const u64 poll_n =
            __atomic_add_fetch(&g_supervisor_poll_debug_count, 1ull, __ATOMIC_RELAXED);
        const u32 pending_dbg =
            __atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_ACQUIRE);

        if (pending_dbg || poll_n < 32 || ((poll_n & 0xfffull) == 0)) {
            ANKO_LOGF_WARN("kernel/sched",
                           "supervisor-poll enter count=%llx cur_pid=%u target_pid=%u "
                           "pending=%u event_pid=%u status=%llx",
                           static_cast<unsigned long long>(poll_n),
                           p ? p->pid : 0,
                           supervisor ? supervisor->pid : 0,
                           pending_dbg,
                           supervisor ? supervisor->supervisor_event_pid : 0,
                           static_cast<unsigned long long>(
                               supervisor ? supervisor->supervisor_event_status : 0));
            debug_log_supervisor_threads(supervisor, "poll_enter");
        }

        if (!pending_dbg) {
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

s = s[:start] + replacement + s[end:]
p.write_text(s)
PY

cmake --build build

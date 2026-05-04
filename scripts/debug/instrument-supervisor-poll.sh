#!/usr/bin/env bash
set -euo pipefail

FILE="kernel/syscall/dispatch.cc"
cp "$FILE" "$FILE.bak.supervisor-poll-debug.$(date +%Y%m%d-%H%M%S)"

python3 - <<'PY'
from pathlib import Path

p = Path("kernel/syscall/dispatch.cc")
s = p.read_text()

# Add poll counter near syscall globals.
if "g_supervisor_poll_debug_count" not in s:
    s = s.replace(
        "u64 g_el0_fault_count = 0;\n",
        "u64 g_el0_fault_count = 0;\nu64 g_supervisor_poll_debug_count = 0;\n",
        1,
    )

# Add a helper to dump supervisor thread state.
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
    # Put it before record_supervisor_event.
    s = s.replace(
        "\nvoid record_supervisor_event(Process* proc, long status) {",
        "\n" + helper + "\nvoid record_supervisor_event(Process* proc, long status) {",
        1,
    )

# After publishing, dump init/supervisor thread state.
if 'debug_log_supervisor_threads(supervisor, "after_publish");' not in s:
    s = s.replace(
        "    supervisor_event_unlock(supervisor);\n    wake_supervisor_process(supervisor);\n",
        '''    debug_log_supervisor_threads(supervisor, "after_publish_before_wake");
    supervisor_event_unlock(supervisor);
    wake_supervisor_process(supervisor);
    debug_log_supervisor_threads(supervisor, "after_publish_after_wake");
''',
        1,
    )

# Add entry logging inside SYS_SUPERVISOR_POLL after supervisor lookup.
needle = '''        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);

        if (__atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_ACQUIRE) == 0) {
            return err(E_NOTFOUND);
        }
'''

replace = '''        auto* supervisor = static_cast<Process*>(supervisor_e->target);
        if (!supervisor) return err(E_NOTFOUND);

        const u64 poll_n = __atomic_add_fetch(&g_supervisor_poll_debug_count, 1ull, __ATOMIC_RELAXED);
        const u32 pending_dbg = __atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_ACQUIRE);

        if (pending_dbg || poll_n < 16 || ((poll_n & 0xfffull) == 0)) {
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
'''

if needle not in s:
    raise SystemExit("could not find SYS_SUPERVISOR_POLL entry block to patch")

s = s.replace(needle, replace, 1)

p.write_text(s)
PY

cmake --build build

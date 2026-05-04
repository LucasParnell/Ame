#!/usr/bin/env bash
set -euo pipefail

files=(
  kernel/syscall/dispatch.cc
  kernel/sched/runqueue.cc
  kernel/log/log.cc
  userland/init/main.cc
  userland/dango/manager/main.cc
  userland/lib/include/mochi/ring.h
  kernel/arch/arm64/el2.S
)

for f in "${files[@]}"; do
  [[ -f "$f" ]] && cp "$f" "$f.bak.final-cleanup.$(date +%Y%m%d-%H%M%S)"
done

python3 - <<'PY'
from pathlib import Path
import re

def replace_function(src: str, signature: str, replacement: str):
    start = src.find(signature)
    if start < 0:
        return src, False

    brace = src.find("{", start)
    if brace < 0:
        return src, False

    depth = 0
    i = brace
    while i < len(src):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                while end < len(src) and src[end] in " \t":
                    end += 1
                if end < len(src) and src[end] == "\n":
                    end += 1
                return src[:start] + replacement + src[end:], True
        i += 1

    return src, False

# ---------------------------------------------------------------------
# kernel/syscall/dispatch.cc
# Keep per-supervisor event slot, remove debug logs/counters.
# ---------------------------------------------------------------------
p = Path("kernel/syscall/dispatch.cc")
s = p.read_text()

s = re.sub(r"\nu64 g_supervisor_poll_debug_count = 0;\n", "\n", s)

s, _ = replace_function(
    s,
    "static void debug_log_supervisor_threads(Process* supervisor, const char* where)",
    "",
)

clean_record = r'''void record_supervisor_event(Process* proc, long status) {
    if (!proc || proc->supervisor_pid == 0) {
        return;
    }

    Process* supervisor = sched::find_process(proc->supervisor_pid);
    if (!supervisor) {
        return;
    }

    bool published = false;

    supervisor_event_lock(supervisor);

    if (__atomic_load_n(&supervisor->supervisor_event_pending, __ATOMIC_RELAXED) == 0) {
        supervisor->supervisor_event_pid = proc->pid;
        supervisor->supervisor_event_status = status;
        __atomic_store_n(&supervisor->supervisor_event_pending, 1u, __ATOMIC_RELEASE);
        published = true;
    }

    supervisor_event_unlock(supervisor);

    if (published) {
        wake_supervisor_process(supervisor);
    }
}
'''

s, ok = replace_function(s, "void record_supervisor_event(Process* proc, long status)", clean_record)
if not ok:
    raise SystemExit("could not replace record_supervisor_event()")

poll_start = s.find("    case SYS_SUPERVISOR_POLL:")
poll_end = s.find("    case SYS_THREAD_CREATE:", poll_start)
if poll_start < 0 or poll_end < 0:
    raise SystemExit("could not find SYS_SUPERVISOR_POLL case")

clean_poll = r'''    case SYS_SUPERVISOR_POLL: {
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

        return ok(static_cast<long>(dead_pid), status);
    }

'''

s = s[:poll_start] + clean_poll + s[poll_end:]

# Remove fault addrspace debug, if still present.
s = re.sub(
    r"\nstatic u64 read_ttbr0_el1_debug\(\) \{.*?"
    r"\nstatic void log_fault_addrspace_debug\(Process\* p, Thread\* t, TrapFrame\* tf, u64 far\) \{.*?\n\}\n",
    "\n",
    s,
    flags=re.S,
)
s = s.replace("        log_fault_addrspace_debug(p, ct, tf, far);\n", "")

p.write_text(s)

# ---------------------------------------------------------------------
# kernel/sched/runqueue.cc
# Remove hot-path frame-owner instrumentation and switch spam.
# ---------------------------------------------------------------------
p = Path("kernel/sched/runqueue.cc")
s = p.read_text()

s = re.sub(
    r"\nstatic bool process_maps_elr\(Process\* p, u64 elr\) \{.*?"
    r"\nstatic void debug_check_frame_owner\(const char\* where, Thread\* cur, TrapFrame\* frame\) \{.*?\n\}\n",
    "\n",
    s,
    flags=re.S,
)

s = s.replace('    debug_check_frame_owner("before_save_cur_tf", cur, frame);\n', "")
s = s.replace('    debug_check_frame_owner("before_restore_next_tf", next, &next->tf);\n', "")

s = re.sub(
    r"\n    // Do not log every context switch\..*?\n#endif\n",
    "\n",
    s,
    flags=re.S,
)

switch_log = '''    ANKO_LOGF_INFO("kernel/sched",
                   "switch tid=%u pid=%u elr=%llx state=%u",
                   next->tid, next->pid,
                   static_cast<unsigned long long>(next->tf.elr),
                   static_cast<unsigned>(next->state));
'''
s = s.replace(switch_log, "")

p.write_text(s)

# ---------------------------------------------------------------------
# kernel/log/log.cc
# Keep TPIDR CPU index and remove unused drop-notice helper/call.
# ---------------------------------------------------------------------
p = Path("kernel/log/log.cc")
s = p.read_text()

cpu_index = r'''u32 cpu_index() {
    u64 id = 0;
    asm volatile("mrs %0, TPIDR_EL1" : "=r"(id));
    return id < kMaxCpus ? static_cast<u32>(id) : 0;
}
'''

s, ok = replace_function(s, "u32 cpu_index()", cpu_index)
if not ok:
    print("warning: could not normalize log cpu_index()")

s, _ = replace_function(s, "void emit_drop_notice(u32 cpu)", "")
s, _ = replace_function(s, "[[maybe_unused]] void emit_drop_notice(u32 cpu)", "")

s = re.sub(r"\n\s*emit_drop_notice\(cpu\);\n", "\n", s)

p.write_text(s)

# ---------------------------------------------------------------------
# userland/init/main.cc
# Remove DBG markers; keep sys_kill-based supervision.
# Restore normal console_log path now that ring notify is fixed.
# ---------------------------------------------------------------------
p = Path("userland/init/main.cc")
s = p.read_text()

s, _ = replace_function(s, "static void debug_marker(const char* msg)", "")

normal_console_log = r'''static void console_log(const char* msg) {
    mochi::log_raw_marker(msg);
}
'''
s, ok = replace_function(s, "static void console_log(const char* msg)", normal_console_log)
if not ok:
    print("warning: could not normalize console_log()")

s = re.sub(r'\n\s*debug_marker\("DBG init: .*?\\n"\);\n', "\n", s)
s = s.replace("constexpr uint32_t MSG_CRASH      = 0xDEAD;\n", "")

# Clean the supervision comment if present.
s = s.replace(
'''    // Supervision demo: kill pong through the process/supervisor path.
    //
    // Do not inject MSG_CRASH through ping_to_pong_ring here.  That ring is
    // SPSC and belongs to demo.ping -> demo.pong; using it from init as a
    // second producer makes the supervision test nondeterministic.
''',
'''    // Supervision demo: kill pong through the process/supervisor path, then
    // restart it according to the manifest policy.
'''
)

p.write_text(s)

# ---------------------------------------------------------------------
# userland/dango/manager/main.cc
# Remove VM heartbeat instrumentation; keep runner yield.
# ---------------------------------------------------------------------
p = Path("userland/dango/manager/main.cc")
s = p.read_text()

if "uint64_t poll_iters = 0;" in s and "dango: vm wait heartbeat" in s:
    start = s.find("    bool logged_agent = false;")
    if start < 0:
        raise SystemExit("could not find VM poll loop start")

    # This loop is intentionally the final loop in manager main.
    end_marker = "        sys_yield();\n    }\n"
    end = s.find(end_marker, start)
    if end < 0:
        raise SystemExit("could not find VM poll loop end")
    end += len(end_marker)

    clean_loop = r'''    bool logged_agent = false;
    bool logged_hello = false;
    while (true) {
        if (have_linux && !logged_hello) {
            const int64_t saw_hello = sys_debug_counter(dango::VM_DEBUG_GUEST_LINUX_HELLO);
            const int64_t saw_agent = sys_debug_counter(dango::VM_DEBUG_GUEST_AGENT_START);
            if (saw_hello > 0) {
                dango_log("dango: linux guest responded\n");
                logged_hello = true;
            } else if (!logged_agent && saw_agent > 0) {
                dango_log("dango: linux guest agent started\n");
                logged_agent = true;
            }
        }
        sys_yield();
    }
'''
    s = s[:start] + clean_loop + s[end:]

p.write_text(s)

# ---------------------------------------------------------------------
# userland/lib/include/mochi/ring.h
# Keep correctness-first notification, remove DEBUG wording.
# ---------------------------------------------------------------------
p = Path("userland/lib/include/mochi/ring.h")
s = p.read_text()

notify_block = r'''    (void)__atomic_exchange_n(&r->flags.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL);

    // Always notify after publishing.  Doorbells coalesce naturally, and this
    // avoids a lost-wake race in the sleep-flag fast path.
    (void)sys_notify(doorbell_cap);

    return true;
'''

s = re.sub(
    r'''    const uint32_t claimed_sleep =
        __atomic_exchange_n\(&r->flags\.receiver_about_to_sleep, 0u, __ATOMIC_ACQ_REL\);
.*?    return true;
''',
    notify_block,
    s,
    count=1,
    flags=re.S,
)

p.write_text(s)

# ---------------------------------------------------------------------
# kernel/arch/arm64/el2.S
# Clean WFI/WFE comment; keep traps enabled.
# ---------------------------------------------------------------------
p = Path("kernel/arch/arm64/el2.S")
s = p.read_text()

s = s.replace(
'''    // Trap guest WFI/WFE.  The VMM currently injects virtual timer state from
    // the sys_vcpu_run() loop; if the guest is allowed to sleep in WFI, it can
    // stop before the agent/init path runs.  The C++ WFx handler advances PC
    // and returns to the async runner instead of spinning in one syscall.
    orr     x0, x0, #(1 << 13)   // TWI: trap guest WFI
    orr     x0, x0, #(1 << 14)   // TWE: trap guest WFE
''',
'''    // Trap guest WFI/WFE so the host VMM can account idle exits and advance
    // virtual timer/IRQ state from the vCPU run loop.
    orr     x0, x0, #(1 << 13)   // TWI: trap guest WFI
    orr     x0, x0, #(1 << 14)   // TWE: trap guest WFE
'''
)

s = s.replace(
'''    msr     VTTBR_EL2, xzr
    // Do not invalidate all stage-1+stage-2 translations on every guest exit.
    // The exit path runs for idle/timer exits too; doing VMALLS12E1 here can
    // wedge in the following barrier and is far too expensive.  Stage-2 TLB
    // invalidation belongs on stage-2 map/unmap or before guest entry when the
    // stage-2 root/generation changes.
    isb
''',
'''    msr     VTTBR_EL2, xzr
    isb
'''
)

p.write_text(s)

PY

cmake --build build

echo
echo "final cleanup applied"

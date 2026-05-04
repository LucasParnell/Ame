// kernel/ipc/doorbell.cc — doorbell: notify and wait_doorbell.
//
// Spec ref: §5.9 Doorbells, §13 Synchronous Handoff
//
// V2 changes:
//   - notify_object uses apply_wake_metadata + target_cpu_for_wake for IPC
//     wake credit and wake-affine placement.
//   - notify_handoff: same-CPU synchronous handoff; falls back to notify_object
//     if eligibility checks fail.
//   - reply_handoff: receiver unblocks sender and requests a switch.
//
// Locking discipline: per-doorbell spinlock held with IRQs masked.
// Cross-CPU wake routes through the target CPU's pending_wakes stack.

#include "anko/ipc.h"
#include "anko/sched.h"
#include "anko/cap.h"
#include "anko/mm.h"
#include "libk/metric.h"
#include "libk/string.h"

namespace ipc {

static u64 g_sleep_count = 0;
static u64 g_wake_count = 0;
static u64 g_wait_calls = 0;
static u64 g_wait_immediate = 0;
static u64 g_notify_calls = 0;
static u64 g_notify_no_waiter = 0;
static u64 g_notify_woke_thread = 0;
static u64 g_doorbell_local_wakes = 0;
static u64 g_doorbell_remote_wakes = 0;

using libk::metric_inc;
using libk::metric_load;

static inline u64 local_irq_save_disable() {
    u64 daif = 0;
    asm volatile("mrs %0, DAIF" : "=r"(daif) :: "memory");
    asm volatile("msr DAIFSet, #2" ::: "memory"); // mask IRQ only
    return daif;
}

static inline void local_irq_restore(u64 daif) {
    asm volatile("msr DAIF, %0" :: "r"(daif) : "memory");
}

static void lock(Doorbell* db) {
    while (__atomic_exchange_n(&db->lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        asm volatile("yield" ::: "memory");
    }
}

static void unlock(Doorbell* db) {
    __atomic_store_n(&db->lock, 0u, __ATOMIC_RELEASE);
}

static CapEntry* resolve_doorbell_entry(u32 pid, CapHandle handle, u32 need_rights) {
    Process* p = sched::find_process(pid);
    if (!p || !p->cap_table) return nullptr;

    CapEntry* e = cap::lookup(*p->cap_table, handle, CAP_DOORBELL);
    if (!e) return nullptr;
    if ((e->rights_mask & need_rights) != need_rights) return nullptr;
    return e;
}

Doorbell* doorbell_create_object(u32 pid) {
    Process* owner = sched::find_process(pid);
    if (!owner || !owner->cap_table) return nullptr;

    Doorbell* db = static_cast<Doorbell*>(mm::heap_alloc(sizeof(Doorbell), alignof(Doorbell)));
    if (!db) return nullptr;
    libk::memset(db, 0, sizeof(*db));
    db->owner_pid = pid;
    db->rights = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;

    return db;
}

CapHandle doorbell_cap_alloc(u32 target_pid, Doorbell* db, u32 rights) {
    if (!db) return kNullCap;
    Process* target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    return cap::alloc(*target->cap_table, CAP_DOORBELL, db, requested);
}

CapHandle doorbell_create(u32 pid) {
    Doorbell* db = doorbell_create_object(pid);
    return doorbell_cap_alloc(pid, db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP);
}

CapHandle doorbell_share(u32 owner_pid, CapHandle db_cap, u32 target_pid, u32 rights) {
    CapEntry* src = resolve_doorbell_entry(owner_pid, db_cap, RIGHT_DUP);
    if (!src) return kNullCap;

    Doorbell* db = static_cast<Doorbell*>(src->target);
    if (!db) return kNullCap;

    Process* target = sched::find_process(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    if ((requested & src->rights_mask) != requested) return kNullCap;

    return doorbell_cap_alloc(target_pid, db, requested);
}

SysError notify_object(Doorbell* db) {
    metric_inc(g_notify_calls);
    if (!db) return E_NOTFOUND;

    Thread* waiter = nullptr;
    const u64 irq = local_irq_save_disable();
    lock(db);

    __atomic_add_fetch(&db->counter, 1ull, __ATOMIC_RELEASE);
    waiter = db->waiter;
    db->waiter = nullptr;

    if (waiter) {
        __atomic_store_n(&waiter->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
        __atomic_add_fetch(&db->wake_count, 1ull, __ATOMIC_RELAXED);
        metric_inc(g_wake_count);
        metric_inc(g_notify_woke_thread);
    } else {
        metric_inc(g_notify_no_waiter);
    }

    unlock(db);
    local_irq_restore(irq);

    if (waiter) {
        sched::apply_wake_metadata(waiter, WAKE_DOORBELL, db->sched_hints);
        const u32 target = sched::target_cpu_for_wake(
            waiter, sched::current(), db->sched_hints, db->home_cpu);
        if (target == sched::current_cpu_id()) {
            metric_inc(g_doorbell_local_wakes);
            sched::wake_local(waiter);
        } else {
            metric_inc(g_doorbell_remote_wakes);
            sched::wake_remote(waiter, target);
        }
    }
    return E_OK;
}

SysError notify(CapHandle handle, u32 caller_pid) {
    CapEntry* e = resolve_doorbell_entry(caller_pid, handle, RIGHT_NOTIFY);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    return notify_object(db);
}

// ---------------------------------------------------------------------------
// notify_handoff — synchronous request/reply IPC (spec §13)
//
// Fast path: sender and receiver are on the same CPU and the doorbell has
// IPC_HINT_HANDOFF_OK set.  The sender blocks, receiver runs immediately via
// the handoff_next slot in post_syscall.
//
// Slow path (any eligibility check fails): fall back to a regular notify and
// return E_OK so the sender continues normally.
// ---------------------------------------------------------------------------
SysError notify_handoff(CapHandle handle, u64 payload_word, u32 caller_pid) {
    metric_inc(g_notify_calls);

    CapEntry* ce = resolve_doorbell_entry(caller_pid, handle, RIGHT_NOTIFY);
    if (!ce) return E_NOTFOUND;
    Doorbell* db = static_cast<Doorbell*>(ce->target);
    if (!db) return E_NOTFOUND;

    Thread* sender = sched::current();
    if (!sender) return E_DEAD;

    Thread* receiver = nullptr;
    bool do_handoff = false;

    const u64 irq = local_irq_save_disable();
    lock(db);

    __atomic_add_fetch(&db->counter, 1ull, __ATOMIC_RELEASE);
    receiver = db->waiter;
    db->waiter = nullptr;

    if (receiver) {
        __atomic_store_n(&receiver->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
        __atomic_add_fetch(&db->wake_count, 1ull, __ATOMIC_RELAXED);

        // Handoff eligibility (spec §13.3):
        //   - IPC_HINT_HANDOFF_OK must be set on the doorbell
        //   - receiver must be sleeping on this doorbell
        //   - same CPU as sender (same-CPU fast path only)
        //   - receiver affinity allows the current CPU
        //   - receiver is not already in a handoff chain (no reply_target)
        //   - receiver's principal is not throttled
        const u32 cpu = sched::current_cpu_id();
        const bool affinity_ok = (receiver->affinity_mask == 0) ||
                                 ((receiver->affinity_mask >> cpu) & 1u);
        if ((db->sched_hints & IPC_HINT_HANDOFF_OK) &&
            receiver->state == TS_SLEEPING &&
            receiver->cpu_id == cpu &&
            affinity_ok &&
            receiver->reply_target == nullptr &&
            !sched::principal_throttled(receiver->principal_id)) {
            __atomic_add_fetch(&db->handoff_count, 1ull, __ATOMIC_RELAXED);
            do_handoff = true;
        }
    } else {
        metric_inc(g_notify_no_waiter);
    }

    unlock(db);
    local_irq_restore(irq);

    if (!do_handoff) {
        // Fallback: wake receiver (if any) via the normal path and return.
        if (receiver) {
            metric_inc(g_wake_count);
            metric_inc(g_notify_woke_thread);
            sched::apply_wake_metadata(receiver, WAKE_DOORBELL, db->sched_hints);
            const u32 target = sched::target_cpu_for_wake(
                receiver, sender, db->sched_hints, db->home_cpu);
            if (target == sched::current_cpu_id()) {
                metric_inc(g_doorbell_local_wakes);
                sched::wake_local(receiver);
            } else {
                metric_inc(g_doorbell_remote_wakes);
                sched::wake_remote(receiver, target);
            }
        }
        return E_OK;
    }

    // --- Synchronous handoff ---
    metric_inc(g_wake_count);
    metric_inc(g_notify_woke_thread);
    metric_inc(g_doorbell_local_wakes);

    // Deliver payload_word to receiver in x1 (x0 will hold the counter on return
    // from wait_doorbell, but handoff receivers conventionally read x1 for payload).
    receiver->tf.x[1] = payload_word;

    // Non-transitive priority donation: lend our vruntime as a lower-bound key
    // so the receiver is not unfairly penalized for being woken late.
    receiver->donated_priority = sender->vruntime_ns;
    receiver->has_donation = true;

    // Cross-link for reply path.
    sender->state = TS_BLOCKED;
    sender->reply_target = receiver;
    receiver->reply_target = sender;

    // Grant IPC wake credit and set latency metadata on receiver.
    sched::apply_wake_metadata(receiver, WAKE_DOORBELL, db->sched_hints);

    // Install receiver as the next thread in post_syscall (same-CPU bypass of pick_next).
    sched::set_handoff_next(receiver);
    sched::request_switch(false, SWITCH_HANDOFF);

    return E_OK;
}

// ---------------------------------------------------------------------------
// reply_handoff — called by the handoff receiver to unblock the sender.
//
// Writes reply_val into sender's x0 trapframe register so that when the
// sender resumes, sys_notify_handoff() returns reply_val.
// ---------------------------------------------------------------------------
SysError reply_handoff(u64 reply_val, u32 caller_pid) {
    (void)caller_pid;
    Thread* cur = sched::current();
    if (!cur) return E_DEAD;

    Thread* sender = cur->reply_target;
    if (!sender) return E_INVALID;
    if (sender->state != TS_BLOCKED) return E_INVALID;

    // Write reply into sender's saved x0 so it sees reply_val on resume.
    sender->tf.x[0] = reply_val;

    // Release donation.
    cur->has_donation = false;
    cur->donated_priority = 0;
    cur->reply_target = nullptr;
    sender->reply_target = nullptr;

    // Re-enqueue sender on the local CPU (same-CPU handoff guarantee).
    sched::wake_local(sender);

    // Requeue current thread and switch: post_syscall picks winner.
    sched::request_switch(true, SWITCH_REPLY);
    return E_OK;
}

SysError wait_doorbell(CapHandle handle, u64 expected, u32 caller_pid) {
    metric_inc(g_wait_calls);

    CapEntry* e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db) return E_NOTFOUND;

    Thread* cur = sched::current();
    if (!cur) return E_DEAD;

    const u64 irq = local_irq_save_disable();
    lock(db);

    const u64 now = __atomic_load_n(&db->counter, __ATOMIC_ACQUIRE);
    if (now != expected) {
        metric_inc(g_wait_immediate);
        unlock(db);
        local_irq_restore(irq);
        return E_OK;
    }

    if (db->waiter != nullptr) {
        unlock(db);
        local_irq_restore(irq);
        return E_BUSY;
    }

    db->waiter = cur;
    __atomic_store_n(&cur->waiting_doorbell, db, __ATOMIC_RELEASE);
    cur->state = TS_SLEEPING;
    metric_inc(g_sleep_count);
    sched::block(cur);

    unlock(db);
    local_irq_restore(irq);
    return E_OK;
}

u64 doorbell_counter(CapHandle handle, u32 caller_pid) {
    CapEntry* e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db) return 0;
    return __atomic_load_n(&db->counter, __ATOMIC_ACQUIRE);
}

void doorbell_clear_waiter(Thread* thread) {
    if (!thread) return;

    Doorbell* db = __atomic_load_n(&thread->waiting_doorbell, __ATOMIC_ACQUIRE);
    if (!db) return;

    const u64 irq = local_irq_save_disable();
    lock(db);
    if (db->waiter == thread) db->waiter = nullptr;
    // Re-load under lock: notify() may have cleared waiting_doorbell between
    // our initial load above and acquiring the lock.
    Doorbell* waiting = __atomic_load_n(&thread->waiting_doorbell, __ATOMIC_ACQUIRE);
    if (waiting == db) {
        __atomic_store_n(&thread->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
    }
    unlock(db);
    local_irq_restore(irq);
}

u64 doorbell_sleep_count()       { return metric_load(g_sleep_count); }
u64 doorbell_wake_count()        { return metric_load(g_wake_count); }
u64 doorbell_wait_calls()        { return metric_load(g_wait_calls); }
u64 doorbell_wait_immediate()    { return metric_load(g_wait_immediate); }
u64 notify_calls()               { return metric_load(g_notify_calls); }
u64 notify_no_waiter()           { return metric_load(g_notify_no_waiter); }
u64 notify_woke_thread()         { return metric_load(g_notify_woke_thread); }
u64 doorbell_local_wake_count()  { return metric_load(g_doorbell_local_wakes); }
u64 doorbell_remote_wake_count() { return metric_load(g_doorbell_remote_wakes); }

} // namespace ipc

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
#include "anko/panic.h"
#include "anko/interrupt_mask.h"
#include "anko/spinlock.h"
#include "libk/metric.h"
#include "libk/string.h"
#include "../sched/sched_internal.h"
#include "ipc_internal.h"

namespace ipc {

static void destroy_doorbell(void* target) {
    auto* db = static_cast<Doorbell*>(target);
    ANKO_ASSERT(!db->waiter);
    mm::slab_free(db, sizeof(Doorbell), alignof(Doorbell));
}


// Each CPU writes one statistics line. Totals are formed only on diagnostic
// reads, so unrelated channels no longer bounce global metric cache lines.
// wake_count and notify_woke_thread have always counted the same event.
struct alignas(64) IpcMetrics {
    u64 sleeps, waits, immediate, notifies, no_waiter, wakes, local, remote;
};
static_assert(sizeof(IpcMetrics) == 64);
static IpcMetrics metrics[kMaxCpus]{};
static IpcMetrics& local_metrics() { return metrics[sched::current_cpu_id()]; }
static u64 metric_total(u64 IpcMetrics::* field) {
    u64 total = 0;
    for (auto& cpu : metrics) total += libk::metric_load(cpu.*field);
    return total;
}

using libk::metric_inc;

static inline u64 local_irq_save_disable() {
    u64 daif = 0;
    asm volatile("mrs %0, DAIF" : "=r"(daif) :: "memory");
    asm volatile("msr DAIFSet, #2" ::: "memory"); // mask IRQ only
    return daif;
}

static inline void local_irq_restore(u64 daif) {
    asm volatile("msr DAIF, %0" :: "r"(kernel::host_interrupt_state(daif)) : "memory");
}

static void lock(Doorbell* db) {
    while (__atomic_exchange_n(&db->lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        asm volatile("yield" ::: "memory");
    }
}

static void unlock(Doorbell* db) {
    __atomic_store_n(&db->lock, 0u, __ATOMIC_RELEASE);
}

static cap::Ref resolve_doorbell_entry(u32 pid, CapHandle handle, u32 need_rights) {
    return resolve_entry(pid, handle, CAP_DOORBELL, need_rights);
}

Doorbell* doorbell_create_object(u32 pid) {
    auto owner = process_reference(pid);
    if (!owner || !owner->cap_table) return nullptr;

    Doorbell* db = static_cast<Doorbell*>(mm::slab_alloc(sizeof(Doorbell), alignof(Doorbell)));
    if (!db) return nullptr;
    libk::memset(db, 0, sizeof(*db));
    db->owner_pid = pid;
    db->rights = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;

    db->lifetime = object::create(db, destroy_doorbell);
    if (!db->lifetime) { destroy_doorbell(db); return nullptr; }
    return db;
}

CapHandle doorbell_cap_alloc(u32 target_pid, Doorbell* db, u32 rights) {
    if (!db) return kNullCap;
    auto target = process_reference(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    return cap::alloc(*target->cap_table, CAP_DOORBELL, db, requested, db->lifetime);
}

CapHandle doorbell_create(u32 pid) {
    Doorbell* db = doorbell_create_object(pid);
    if (!db) return kNullCap;
    CapHandle handle = doorbell_cap_alloc(pid, db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP);
    object::release(db->lifetime); // transfer the creator root to the capability
    return handle;
}

CapHandle doorbell_share(u32 owner_pid, CapHandle db_cap, u32 target_pid, u32 rights) {
    cap::Ref src = resolve_doorbell_entry(owner_pid, db_cap, RIGHT_DUP);
    if (!src) return kNullCap;

    Doorbell* db = static_cast<Doorbell*>(src->target);
    if (!db) return kNullCap;

    auto target = process_reference(target_pid);
    if (!target || !target->cap_table) return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0) return kNullCap;
    if ((requested & src->rights_mask) != requested) return kNullCap;

    return cap::alloc(*target->cap_table, CAP_DOORBELL, db, requested, src->object);
}

SysError notify_object(Doorbell* db) {
    auto& stats = local_metrics();
    metric_inc(stats.notifies);
    if (!db) return E_NOTFOUND;

    Thread* waiter = nullptr;
    object::Ref<Thread> waiter_reference;
    const u64 irq = local_irq_save_disable();
    lock(db);

    __atomic_add_fetch(&db->counter, 1ull, __ATOMIC_RELEASE);
    waiter = db->waiter;
    waiter_reference.reset(waiter, waiter ? waiter->lifetime : nullptr);
    db->waiter = nullptr;

    if (waiter) {
        __atomic_store_n(&waiter->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
        ++db->wake_count;
        metric_inc(stats.wakes);
    } else {
        metric_inc(stats.no_waiter);
    }

    unlock(db);
    local_irq_restore(irq);

    if (waiter) {
        const u32 target = sched::wake_ipc(
            waiter, sched::current(), db->sched_hints, db->home_cpu);
        if (target == sched::current_cpu_id()) {
            metric_inc(stats.local);
        } else {
            metric_inc(stats.remote);
        }
    }
    return E_OK;
}

SysError notify(CapHandle handle, u32 caller_pid) {
    cap::Ref e = resolve_doorbell_entry(caller_pid, handle, RIGHT_NOTIFY);
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
    auto& stats = local_metrics();
    metric_inc(stats.notifies);

    cap::Ref ce = resolve_doorbell_entry(caller_pid, handle, RIGHT_NOTIFY);
    if (!ce) return E_NOTFOUND;
    Doorbell* db = static_cast<Doorbell*>(ce->target);
    if (!db) return E_NOTFOUND;

    Thread* sender = sched::current();
    if (!sender) return E_DEAD;

    Thread* receiver = nullptr;
    object::Ref<Thread> receiver_reference;
    bool do_handoff = false;

    const u64 irq = local_irq_save_disable();
    lock(db);

    __atomic_add_fetch(&db->counter, 1ull, __ATOMIC_RELEASE);
    receiver = db->waiter;
    receiver_reference.reset(receiver, receiver ? receiver->lifetime : nullptr);
    db->waiter = nullptr;

    if (receiver) {
        __atomic_store_n(&receiver->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
        ++db->wake_count;

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
            sender->reply_target == nullptr &&
            !__atomic_load_n(&receiver->terminate_requested, __ATOMIC_ACQUIRE) &&
            receiver->state == TS_SLEEPING &&
            receiver->cpu_id == cpu &&
            affinity_ok &&
            receiver->reply_target == nullptr &&
            !sched::principal_throttled(receiver->principal_id)) {
            ++db->handoff_count;
            do_handoff = true;
        }
    } else {
        metric_inc(stats.no_waiter);
    }

    unlock(db);
    local_irq_restore(irq);

    if (!do_handoff) {
        // Fallback: wake receiver (if any) via the normal path and return.
        if (receiver) {
            metric_inc(stats.wakes);
            const u32 target = sched::wake_ipc(
                receiver, sender, db->sched_hints, db->home_cpu);
            if (target == sched::current_cpu_id()) {
                metric_inc(stats.local);
            } else {
                metric_inc(stats.remote);
            }
        }
        return E_OK;
    }

    // --- Synchronous handoff ---
    metric_inc(stats.wakes);
    metric_inc(stats.local);

    // Deliver payload_word to receiver in x1 (x0 will hold the counter on return
    // from wait_doorbell, but handoff receivers conventionally read x1 for payload).
    receiver->tf.x[1] = payload_word;

    // Non-transitive priority donation: lend our vruntime as a lower-bound key
    // so the receiver is not unfairly penalized for being woken late.
    sched::handoff_grant_donation(receiver, sender);

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
    ANKO_ASSERT(cur->state == TS_RUNNING);

    Thread* sender = cur->reply_target;
    if (!sender) return E_INVALID;
    if (sender->state != TS_BLOCKED) return E_INVALID;

    // Write reply into sender's saved x0 so it sees reply_val on resume.
    sender->tf.x[0] = reply_val;

    // Release donation.
    sched::handoff_clear_donation(cur);
    cur->reply_target = nullptr;
    sender->reply_target = nullptr;

    // Re-enqueue sender on the local CPU (same-CPU handoff guarantee).
    sched::wake_local(sender);

    // Requeue current thread and switch: post_syscall picks winner.
    sched::request_switch(true, SWITCH_REPLY);
    return E_OK;
}

// Timer admission/polling is CPU-owned. A hot occupancy word skips every empty
// slot, deadline keys form a dense column, and retained object links stay in a
// separate cold column. Empty ticks do not read the clock or any object records.
struct DeadlineWait {
    Thread* thread;
    Doorbell* doorbell;
    object::Record* thread_ref;
    object::Record* doorbell_ref;
    u64 sequence;
};
struct alignas(64) DeadlineQueue {
    u64 active;
    alignas(64) u64 deadlines[64];
    DeadlineWait rows[64];
};
static_assert(sizeof(DeadlineWait) == 40);
static_assert(sizeof(DeadlineQueue) == 3136);
static DeadlineQueue deadline_waits[kMaxCpus]{};

void poll_deadline_waiters() {
    auto& queue = deadline_waits[sched::current_cpu_id()];
    // An empty poll can return before touching DAIF. Re-read after IRQ masking
    // in the nonempty case, since a timer may reap entries between these reads.
    if (!__atomic_load_n(&queue.active, __ATOMIC_RELAXED)) return;
    kernel::InterruptGuard irq;
    u64 active = queue.active;
    if (!active) return;
    const u64 now = sched::sched_clock_ns();
    while (active) {
        const u32 index = __builtin_ctzll(active);
        active &= active - 1;
        const auto row = queue.rows[index];
        Doorbell* db = row.doorbell;
        lock(db);
        const bool waiting = db->waiter == row.thread &&
            __atomic_load_n(&row.thread->wait_sequence, __ATOMIC_RELAXED) == row.sequence;
        const bool expired = waiting && now >= queue.deadlines[index];
        if (expired) {
            db->waiter = nullptr;
            __atomic_store_n(&row.thread->waiting_doorbell, nullptr, __ATOMIC_RELEASE);
        }
        unlock(db);
        if (expired) {
            sched::apply_wake_metadata(row.thread, WAKE_DOORBELL, db->sched_hints);
            sched::wake(row.thread);
        }
        if (!waiting || expired) {
            // Unpublish before releasing: a final destructor can run arbitrary
            // cold teardown, and the next wait must see an available slot.
            queue.active &= ~(1ull << index);
            queue.rows[index] = {};
            object::release(row.thread_ref);
            object::release(row.doorbell_ref);
        }
    }
}
SysError wait_doorbell_until(CapHandle handle, u64 expected, u64 deadline, u32 caller_pid) {
    auto& stats = local_metrics();
    metric_inc(stats.waits);
    cap::Ref e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db) return E_NOTFOUND;
    Thread* cur = sched::current();
    if (!cur) return E_DEAD;
    ANKO_ASSERT(cur->state == TS_RUNNING);
    if (!deadline && __atomic_load_n(&db->counter, __ATOMIC_ACQUIRE) != expected) {
        metric_inc(stats.immediate);
        return E_OK;
    }
    const u64 irq = local_irq_save_disable();
    // Only timed admission touches the local timer table. Untimed Tier-2 waits
    // retain their original constant work; the periodic tick reaps stale rows.
    if (deadline) poll_deadline_waiters();
    auto& queue = deadline_waits[sched::current_cpu_id()];
    u32 timer = 64;
    if (deadline) {
        if (sched::sched_clock_ns() >= deadline) { local_irq_restore(irq); return E_OK; }
    }
    // A changed counter already satisfies the wait. Keep this common syscall
    // return off the doorbell lock and timer admission path; recheck after
    // locking below to cover a notify racing with actual sleep registration.
    if (__atomic_load_n(&db->counter, __ATOMIC_ACQUIRE) != expected) {
        metric_inc(stats.immediate);
        local_irq_restore(irq); return E_OK;
    }
    if (deadline) {
        const u64 available = ~queue.active;
        if (!available) { local_irq_restore(irq); return E_OVERFLOW; }
        timer = __builtin_ctzll(available);
    }
    lock(db);
    if (__atomic_load_n(&db->counter, __ATOMIC_ACQUIRE) != expected) {
        metric_inc(stats.immediate);
        unlock(db); local_irq_restore(irq); return E_OK;
    }
    if (db->waiter) { unlock(db); local_irq_restore(irq); return E_BUSY; }
    auto* previous_wait = cur->waiting_lifetime;
    cur->waiting_lifetime = e->object;
    object::retain(cur->waiting_lifetime);
    const u64 sequence = __atomic_add_fetch(&cur->wait_sequence, 1ull, __ATOMIC_RELAXED);
    if (timer < 64) {
        auto* thread_ref = cur->lifetime;
        if (thread_ref) object::retain(thread_ref);
        else thread_ref = object::acquire(cur); // caller-owned host fixtures
        object::retain(e->object);
        queue.rows[timer] = {cur, db, thread_ref, e->object, sequence};
        queue.deadlines[timer] = deadline;
        queue.active |= 1ull << timer;
    }
    db->waiter = cur;
    __atomic_store_n(&cur->waiting_doorbell, db, __ATOMIC_RELEASE);
    cur->state = TS_SLEEPING;
    metric_inc(stats.sleeps);
    sched::block(cur);
    unlock(db);
    object::release(previous_wait); // final destruction runs outside this doorbell's lock
    local_irq_restore(irq);
    return E_OK;
}
SysError wait_doorbell(CapHandle handle, u64 expected, u32 caller_pid) {
    return wait_doorbell_until(handle, expected, 0, caller_pid);
}

u64 doorbell_counter(CapHandle handle, u32 caller_pid) {
    cap::Ref e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db) return 0;
    return __atomic_load_n(&db->counter, __ATOMIC_ACQUIRE);
}

void doorbell_clear_waiter(Thread* thread) {
    if (!thread) return;

    Doorbell* db = __atomic_load_n(&thread->waiting_doorbell, __ATOMIC_ACQUIRE);
    if (!db) {
        object::release(thread->waiting_lifetime); thread->waiting_lifetime = nullptr;
        return;
    }

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
    object::release(thread->waiting_lifetime); thread->waiting_lifetime = nullptr;
}

u64 doorbell_sleep_count()       { return metric_total(&IpcMetrics::sleeps); }
u64 doorbell_wake_count()        { return metric_total(&IpcMetrics::wakes); }
u64 doorbell_wait_calls()        { return metric_total(&IpcMetrics::waits); }
u64 doorbell_wait_immediate()    { return metric_total(&IpcMetrics::immediate); }
u64 notify_calls()               { return metric_total(&IpcMetrics::notifies); }
u64 notify_no_waiter()           { return metric_total(&IpcMetrics::no_waiter); }
u64 notify_woke_thread()         { return metric_total(&IpcMetrics::wakes); }
u64 doorbell_local_wake_count()  { return metric_total(&IpcMetrics::local); }
u64 doorbell_remote_wake_count() { return metric_total(&IpcMetrics::remote); }

} // namespace ipc

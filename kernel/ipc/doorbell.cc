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

#include "libk/atomic.h"
#include "anko/ipc.h"
#include "anko/sched.h"
#include "anko/cap.h"
#include "anko/mm.h"
#include "anko/panic.h"
#include "anko/interrupt_mask.h"
#include "anko/timer.h"
#include "anko/spinlock.h"
#include "libk/metric.h"
#include "libk/cpu.h"
#include "libk/string.h"
#include "libk/deadline.h"
#include "../sched/sched_internal.h"
#include "ipc_internal.h"

namespace ipc {

// Defined after the deadline queue, which it must retire rows from.
static void destroy_doorbell(void* target);

// Each CPU writes one statistics line. Totals are formed only on diagnostic
// reads, so unrelated channels no longer bounce global metric cache lines.
// wake_count and notify_woke_thread have always counted the same event.
struct alignas(64) IpcMetrics {
    u64 sleeps, waits, immediate, notifies, no_waiter, wakes, local, remote;
};
static_assert(sizeof(IpcMetrics) == 64);
static IpcMetrics metrics[kMaxCpus]{};
static IpcMetrics& local_metrics() {
    return metrics[sched::current_cpu_id()];
}
static u64 metric_total(u64 IpcMetrics::* field) {
    u64 total = 0;
    for (auto& cpu : metrics)
        total += libk::metric_load(cpu.*field);
    return total;
}

using libk::metric_inc;

static inline u64 local_irq_save_disable() {
    return libk::cpu::interrupt_save_disable();
}

static inline void local_irq_restore(u64 daif) {
    libk::cpu::interrupt_restore(kernel::host_interrupt_state(daif));
}

static void lock(Doorbell* db) {
    libk::spin_lock(db->lock);
}

static void unlock(Doorbell* db) {
    libk::spin_unlock(db->lock);
}

static cap::Ref resolve_doorbell_entry(u32 pid, CapHandle handle, u32 need_rights) {
    return resolve_entry(pid, handle, CAP_DOORBELL, need_rights);
}

Doorbell* doorbell_create_object(u32 pid) {
    auto owner = process_reference(pid);
    if (!owner || !owner->cap_table)
        return nullptr;

    Doorbell* db = static_cast<Doorbell*>(mm::allocate<Doorbell>());
    if (!db)
        return nullptr;
    libk::memset(db, 0, sizeof(*db));
    db->owner_pid = pid;

    db->lifetime = object::create(db, destroy_doorbell);
    if (!db->lifetime) {
        destroy_doorbell(db);
        return nullptr;
    }
    return db;
}

CapHandle doorbell_cap_alloc(u32 target_pid, Doorbell* db, u32 rights) {
    if (!db)
        return kNullCap;
    auto target = process_reference(target_pid);
    if (!target || !target->cap_table)
        return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0)
        return kNullCap;
    return cap::alloc(*target->cap_table, CAP_DOORBELL, db, requested, db->lifetime);
}

CapHandle doorbell_create(u32 pid, u32 hints) {
    if (hints & ~(IPC_HINT_LATENCY | IPC_HINT_THROUGHPUT | IPC_HINT_WAKE_AFFINE |
                  IPC_HINT_HANDOFF_OK | IPC_HINT_SERVER | IPC_HINT_NO_STEAL))
        return kNullCap;
    Doorbell* db = doorbell_create_object(pid);
    if (!db)
        return kNullCap;
    db->sched_hints = hints;
    CapHandle handle = doorbell_cap_alloc(pid, db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP);
    object::release(db->lifetime); // transfer the creator root to the capability
    return handle;
}

CapHandle doorbell_create_bound(u32 pid, CapHandle server_thread) {
    auto authority = resolve_entry(pid, server_thread, CAP_THREAD, RIGHT_THREAD_CONTROL);
    if (!authority)
        return kNullCap;
    auto* receiver = static_cast<Thread*>(authority->target);
    if (libk::atomic::load_acquire(&receiver->terminate_requested) || receiver->state == TS_DEAD)
        return kNullCap;
    Doorbell* db = doorbell_create_object(pid);
    if (!db)
        return kNullCap;
    db->sched_hints = IPC_HINT_HANDOFF_OK;
    db->bound_server = authority->object;
    object::retain(db->bound_server);
    const auto handle = doorbell_cap_alloc(pid, db, RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP);
    object::release(db->lifetime);
    return handle;
}

CapHandle doorbell_share(u32 owner_pid, CapHandle db_cap, u32 target_pid, u32 rights) {
    cap::Ref src = resolve_doorbell_entry(owner_pid, db_cap, RIGHT_DUP);
    if (!src)
        return kNullCap;

    Doorbell* db = static_cast<Doorbell*>(src->target);
    if (!db)
        return kNullCap;

    auto target = process_reference(target_pid);
    if (!target || !target->cap_table)
        return kNullCap;

    const u32 allowed = RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP;
    const u32 requested = rights & allowed;
    if (requested == 0)
        return kNullCap;
    if ((requested & src->rights_mask) != requested)
        return kNullCap;

    return cap::alloc(*target->cap_table, CAP_DOORBELL, db, requested, src->object);
}

// Every counter writer holds this doorbell's IRQ-masked lock. Readers remain
// atomic and acquire the release publication, but writers need no second LL/SC
// loop inside a lock they already own.
static void advance_counter_locked(Doorbell* db) {
    const u64 counter = libk::atomic::load_relaxed(&db->counter);
    libk::atomic::store_release(&db->counter, counter + 1);
}

SysError notify_object(Doorbell* db) {
    auto& stats = local_metrics();
    metric_inc(stats.notifies);
    if (!db)
        return E_NOTFOUND;

    Thread* waiter = nullptr;
    object::Ref<Thread> waiter_reference;
    const u64 irq = local_irq_save_disable();
    lock(db);

    advance_counter_locked(db);
    waiter = db->waiter;
    waiter_reference.reset(waiter, waiter ? waiter->lifetime : nullptr);
    db->waiter = nullptr;

    if (waiter) {
        libk::atomic::store_release(&waiter->waiting_doorbell, nullptr);
        ++db->wake_count;
        metric_inc(stats.wakes);
        sched::ipc_note_wake(waiter->principal_id);
    } else {
        metric_inc(stats.no_waiter);
    }

    unlock(db);
    local_irq_restore(irq);

    if (waiter) {
        const u32 target = sched::wake_ipc(waiter, sched::current(), db->sched_hints, db->home_cpu);
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
    cap::Ref ce = resolve_doorbell_entry(caller_pid, handle, RIGHT_NOTIFY);
    if (!ce)
        return E_NOTFOUND;
    return notify_handoff_object(static_cast<Doorbell*>(ce->target), payload_word);
}

SysError notify_handoff_object(Doorbell* db, u64 payload_word) {
    auto& stats = local_metrics();
    metric_inc(stats.notifies);
    if (!db)
        return E_NOTFOUND;

    Thread* sender = sched::current();
    if (!sender)
        return E_DEAD;

    Thread* receiver = nullptr;
    object::Ref<Thread> receiver_reference;
    bool do_handoff = false;

    const u64 irq = local_irq_save_disable();
    lock(db);

    advance_counter_locked(db);
    receiver = db->waiter;
    receiver_reference.reset(receiver, receiver ? receiver->lifetime : nullptr);
    db->waiter = nullptr;

    if (receiver) {
        libk::atomic::store_release(&receiver->waiting_doorbell, nullptr);
        ++db->wake_count;

        // Handoff eligibility (spec §13.3):
        //   - IPC_HINT_HANDOFF_OK must be set on the doorbell
        //   - receiver must be sleeping on this doorbell
        //   - same CPU as sender (same-CPU fast path only)
        //   - receiver affinity allows the current CPU
        //   - receiver is not already in a handoff chain (no reply_target)
        //   - receiver's principal is not throttled
        const u32 cpu = sched::current_cpu_id();
        if (receiver != sender) {
            // Context binding may change a sleeping receiver's CPU. Serialize
            // eligibility and link publication with both lifecycle guards;
            // once linked, bind/wake/steal preserve this CPU's ownership.
            Thread* first = reinterpret_cast<usize>(sender) < reinterpret_cast<usize>(receiver)
                                ? sender : receiver;
            Thread* second = first == sender ? receiver : sender;
            kernel::LockGuard first_life(*reinterpret_cast<kernel::SpinLock*>(&first->lifecycle_lock));
            kernel::LockGuard second_life(*reinterpret_cast<kernel::SpinLock*>(&second->lifecycle_lock));
            const bool affinity_ok =
                (receiver->affinity_mask == 0) || ((receiver->affinity_mask >> cpu) & 1u);
            if ((db->sched_hints & IPC_HINT_HANDOFF_OK) &&
                (!db->bound_server || db->bound_server == receiver->lifetime) &&
                !__atomic_load_n(&sender->reply_target, __ATOMIC_ACQUIRE) &&
                !libk::atomic::load_acquire(&receiver->terminate_requested) &&
                receiver->state == TS_SLEEPING && receiver->cpu_id == cpu && affinity_ok &&
                !__atomic_load_n(&receiver->reply_target, __ATOMIC_ACQUIRE) &&
                !sched::principal_throttled(receiver->principal_id)) {
                receiver->tf.x[1] = payload_word;
                sched::handoff_grant_donation(receiver, sender);
                __atomic_store_n(&sender->reply_target, receiver, __ATOMIC_RELEASE);
                __atomic_store_n(&receiver->reply_target, sender, __ATOMIC_RELEASE);
                sender->state = TS_BLOCKED;
                ++db->handoff_count;
                do_handoff = true;
            }
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
            sched::ipc_note_wake(receiver->principal_id);
            const u32 target = sched::wake_ipc(receiver, sender, db->sched_hints, db->home_cpu);
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
    sched::ipc_note_wake(receiver->principal_id);
    metric_inc(stats.local);

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
static SysError complete_reply(Thread* cur, u64 reply_val) {
    Thread* sender = __atomic_load_n(&cur->reply_target, __ATOMIC_ACQUIRE);
    if (!sender)
        return E_INVALID;
    if (sender->state != TS_BLOCKED)
        return E_INVALID;
    object::Ref<Thread> sender_reference(sender, sender->lifetime);

    // Write reply into sender's saved x0 so it sees reply_val on resume.
    sender->tf.x[0] = reply_val;

    // Release donation.
    sched::handoff_clear_donation(cur);
    __atomic_store_n(&cur->reply_target, static_cast<Thread*>(nullptr), __ATOMIC_RELEASE);
    // Queue the sender while its link still fixes its CPU. This publishes a
    // lifecycle-protected runnable state before remote context binding is
    // allowed again. Reply-and-wait can still use wake's sole-receiver slot.
    sched::wake(sender);
    __atomic_store_n(&sender->reply_target, static_cast<Thread*>(nullptr), __ATOMIC_RELEASE);
    // reply-and-wait has already parked the server. Plain reply preserves its
    // historical continue-after-reply behavior by requeueing it.
    sched::request_switch(cur->state == TS_RUNNING, SWITCH_REPLY);
    return E_OK;
}

SysError reply_handoff(u64 reply_val, u32 caller_pid) {
    (void)caller_pid;
    kernel::InterruptGuard irq;
    Thread* cur = sched::current();
    if (!cur)
        return E_DEAD;
    ANKO_ASSERT(cur->state == TS_RUNNING);
    return complete_reply(cur, reply_val);
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

static void update_deadline_timer(const DeadlineQueue& queue) {
    u64 active = queue.active, earliest = 0;
    while (active) {
        const auto index = __builtin_ctzll(active);
        active &= active - 1;
        const auto deadline = queue.deadlines[index];
        earliest = libk::earlier_deadline(earliest, deadline);
    }
    drivers::timer::set_deadline(drivers::timer::DeadlineSource::Ipc, earliest);
}

// A cap revoke can land while a thread is asleep on this doorbell, so the teardown
// must retire the waiter and any deadline row pointing at it. Asserting here let a
// plain revoke take the kernel down, which userland must never be able to do.
static void destroy_doorbell(void* target) {
    auto* db = static_cast<Doorbell*>(target);
    Thread* waiter = db->waiter;
    db->waiter = nullptr;
    if (waiter) {
        libk::atomic::store_release(&waiter->waiting_doorbell, nullptr);
        sched::wake(waiter);
    }
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        auto& queue = deadline_waits[cpu];
        u64 active = libk::atomic::load_acquire(&queue.active);
        if (!active)
            continue;
        for (u32 index = 0; index < 64; ++index) {
            if (!(active & (1ull << index)))
                continue;
            const DeadlineWait row = queue.rows[index];
            if (row.doorbell != db)
                continue;
            queue.rows[index] = {};
            __atomic_and_fetch(&queue.active, ~(1ull << index), __ATOMIC_ACQ_REL);
            object::release(row.thread_ref);
            object::release(row.doorbell_ref);
        }
    }
    object::release(db->bound_server);
    mm::deallocate<Doorbell>(db);
}

void poll_deadline_waiters() {
    auto& queue = deadline_waits[sched::current_cpu_id()];
    // An empty poll can return before touching DAIF. Re-read after IRQ masking
    // in the nonempty case, since a timer may reap entries between these reads.
    if (!libk::atomic::load_relaxed(&queue.active))
        return;
    kernel::InterruptGuard irq;
    u64 active = queue.active;
    if (!active)
        return;
    const u64 now = sched::sched_clock_ns();
    while (active) {
        const u32 index = __builtin_ctzll(active);
        active &= active - 1;
        const auto row = queue.rows[index];
        Doorbell* db = row.doorbell;
        lock(db);
        const bool waiting = db->waiter == row.thread &&
                             libk::atomic::load_relaxed(&row.thread->wait_sequence) == row.sequence;
        const bool expired = waiting && now >= queue.deadlines[index];
        if (expired) {
            db->waiter = nullptr;
            libk::atomic::store_release(&row.thread->waiting_doorbell, nullptr);
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
    update_deadline_timer(queue);
}
SysError wait_doorbell_until(CapHandle handle, u64 expected, u64 deadline, u32 caller_pid) {
    cap::Ref e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    return wait_doorbell_object(e, expected, deadline);
}

SysError wait_doorbell_object(const cap::Ref& e, u64 expected, u64 deadline, const Ring* ring,
                              u64 cursor, bool writable, u32 required_space) {
    auto& stats = local_metrics();
    metric_inc(stats.waits);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db)
        return E_NOTFOUND;
    Thread* cur = sched::current();
    if (!cur)
        return E_DEAD;
    ANKO_ASSERT(cur->state == TS_RUNNING);
    if (required_space && (!ring || !writable || required_space > mx_ring_capacity(ring->order)))
        return E_INVALID;
    const auto changed = [&] {
        if (!ring)
            return libk::atomic::load_acquire(&db->counter) != expected;
        // Kernel-created physical backing is retained by the ring capability;
        // no untrusted VA is dereferenced while holding the doorbell lock.
        const auto* page = reinterpret_cast<const RingHeader*>(ring->phys_base);
        if (writable && required_space) {
            const u64 head = libk::atomic::load_acquire(&page->head.head);
            const u64 tail = libk::atomic::load_acquire(&page->tail.tail);
            const u64 pending = head - tail;
            // Corrupt shared cursors return to userspace validation, never park.
            return pending > mx_ring_capacity(ring->order) ||
                   pending <= mx_ring_capacity(ring->order) - required_space;
        }
        return libk::atomic::load_acquire(writable ? &page->tail.tail : &page->head.head) != cursor;
    };
    if (!deadline && changed()) {
        metric_inc(stats.immediate);
        return E_OK;
    }
    const u64 irq = local_irq_save_disable();
    // Only timed admission touches the local timer table. Untimed Tier-2 waits
    // retain their original constant work; the periodic tick reaps stale rows.
    if (deadline)
        poll_deadline_waiters();
    auto& queue = deadline_waits[sched::current_cpu_id()];
    u32 timer = 64;
    if (deadline) {
        if (sched::sched_clock_ns() >= deadline) {
            local_irq_restore(irq);
            return E_OK;
        }
    }
    // A changed counter already satisfies the wait. Keep this common syscall
    // return off the doorbell lock and timer admission path; recheck after
    // locking below to cover a notify racing with actual sleep registration.
    if (changed()) {
        metric_inc(stats.immediate);
        local_irq_restore(irq);
        return E_OK;
    }
    if (deadline) {
        const u64 available = ~queue.active;
        if (!available) {
            local_irq_restore(irq);
            return E_OVERFLOW;
        }
        timer = __builtin_ctzll(available);
    }
    // A supervisor/lifecycle wake can make this thread runnable without
    // notifying its doorbell. Retire that registration before parking again.
    // Releasing its last reference after installing the new waiter lets the
    // old doorbell's destructor wake us a second time, stranding the new one.
    doorbell_clear_waiter(cur);
    lock(db);
    if (changed()) {
        metric_inc(stats.immediate);
        unlock(db);
        local_irq_restore(irq);
        return E_OK;
    }
    if (db->waiter) {
        unlock(db);
        local_irq_restore(irq);
        return E_BUSY;
    }
    cur->waiting_lifetime = e->object;
    object::retain(cur->waiting_lifetime);
    const u64 sequence = __atomic_add_fetch(&cur->wait_sequence, 1ull, __ATOMIC_RELAXED);
    if (timer < 64) {
        auto* thread_ref = cur->lifetime;
        if (thread_ref)
            object::retain(thread_ref);
        else
            thread_ref = object::acquire(cur); // caller-owned host fixtures
        object::retain(e->object);
        queue.rows[timer] = {cur, db, thread_ref, e->object, sequence};
        queue.deadlines[timer] = deadline;
        queue.active |= 1ull << timer;
        update_deadline_timer(queue);
    }
    db->waiter = cur;
    libk::atomic::store_release(&cur->waiting_doorbell, db);
    cur->state = TS_SLEEPING;
    metric_inc(stats.sleeps);
    sched::ipc_note_park(cur->principal_id);
    sched::block(cur);
    unlock(db);
    local_irq_restore(irq);
    return E_OK;
}

SysError wait_ring(CapHandle ring_handle, CapHandle doorbell, u64 cursor, u32 direction,
                   u32 caller_pid, u32 required_space) {
    const bool writable = direction & MX_RING_WAIT_WRITABLE;
    if (direction & ~(MX_RING_WAIT_WRITABLE | MX_RING_WAIT_MIN_SPACE))
        return E_INVALID;
    if ((direction & MX_RING_WAIT_MIN_SPACE) && (!writable || !required_space))
        return E_INVALID;
    if (!(direction & MX_RING_WAIT_MIN_SPACE))
        required_space = 0;
    const cap::LookupRequest requests[] = {
        {ring_handle, CAP_SHARED_RING, writable ? RIGHT_WRITE : RIGHT_READ},
        {doorbell, CAP_DOORBELL, RIGHT_WAIT}};
    cap::Ref authority[2];
    if (!resolve_entries(caller_pid, requests, authority, 2))
        return E_NOTFOUND;
    return wait_doorbell_object(authority[1], 0, 0, static_cast<Ring*>(authority[0]->target),
                                cursor, writable, required_space);
}

SysError exchange_ring(CapHandle notify_handle, CapHandle ring_handle, CapHandle wait_handle,
                       u64 cursor, u32 flags, u32 caller_pid, u32 required_space) {
    if (flags & ~(MX_IPC_EXCHANGE_HANDOFF | MX_IPC_EXCHANGE_REPLY | MX_IPC_EXCHANGE_WRITABLE))
        return E_INVALID;
    if (flags == (MX_IPC_EXCHANGE_HANDOFF | MX_IPC_EXCHANGE_REPLY))
        return E_INVALID;
    const bool writable = flags & MX_IPC_EXCHANGE_WRITABLE;
    if (writable && (flags != MX_IPC_EXCHANGE_WRITABLE || !required_space))
        return E_INVALID;
    if (!writable)
        required_space = 0; // original ABI ignores unused a5
    const cap::LookupRequest requests[] = {
        {ring_handle, CAP_SHARED_RING, writable ? RIGHT_WRITE : RIGHT_READ},
        {wait_handle, CAP_DOORBELL, RIGHT_WAIT},
        {notify_handle, CAP_DOORBELL, RIGHT_NOTIFY}};
    cap::Ref authority[3];
    // All failure-prone authority checks precede notification/reply side effects.
    if (!resolve_entries(caller_pid, requests, authority, 3))
        return E_NOTFOUND;
    const auto& wait = authority[1];
    const auto* ring = static_cast<Ring*>(authority[0]->target);
    if (writable && required_space > mx_ring_capacity(ring->order))
        return E_INVALID;
    kernel::InterruptGuard irq;
    Thread* cur = sched::current();
    if (!cur)
        return E_DEAD;
    if ((flags & MX_IPC_EXCHANGE_REPLY) &&
        __atomic_load_n(&cur->reply_target, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&cur->reply_target, __ATOMIC_ACQUIRE)->state != TS_BLOCKED)
            return E_INVALID;
        const auto rc = wait_doorbell_object(wait, 0, 0, ring, cursor, false);
        if (rc != E_OK)
            return rc;
        return complete_reply(cur, E_OK);
    }
    auto* db = static_cast<Doorbell*>(authority[2]->target);
    const auto* page = reinterpret_cast<const RingHeader*>(ring->phys_base);
    const bool ready = writable || libk::atomic::load_acquire(&page->head.head) != cursor;
    if (!(flags & MX_IPC_EXCHANGE_HANDOFF) || ready || !(db->sched_hints & IPC_HINT_HANDOFF_OK)) {
        // Authority is validated already. Register the caller before waking
        // its peer, allowing a sole local receiver to bypass heap insertion.
        const auto rc = wait_doorbell_object(wait, 0, 0, ring, cursor, writable, required_space);
        return rc != E_OK ? rc : notify_object(db);
    }
    const auto rc = (flags & MX_IPC_EXCHANGE_HANDOFF) && !ready ? notify_handoff_object(db, 0)
                                                                : notify_object(db);
    if (rc != E_OK || cur->state == TS_BLOCKED)
        return rc;
    return wait_doorbell_object(wait, 0, 0, ring, cursor, writable, required_space);
}
SysError wait_doorbell(CapHandle handle, u64 expected, u32 caller_pid) {
    return wait_doorbell_until(handle, expected, 0, caller_pid);
}

u64 doorbell_counter(CapHandle handle, u32 caller_pid) {
    cap::Ref e = resolve_doorbell_entry(caller_pid, handle, RIGHT_WAIT);
    Doorbell* db = e ? static_cast<Doorbell*>(e->target) : nullptr;
    if (!db)
        return 0;
    return libk::atomic::load_acquire(&db->counter);
}

void doorbell_clear_waiter(Thread* thread) {
    if (!thread)
        return;

    Doorbell* db = libk::atomic::load_acquire(&thread->waiting_doorbell);
    if (!db) {
        object::release(thread->waiting_lifetime);
        thread->waiting_lifetime = nullptr;
        return;
    }

    const u64 irq = local_irq_save_disable();
    lock(db);
    if (db->waiter == thread)
        db->waiter = nullptr;
    // Re-load under lock: notify() may have cleared waiting_doorbell between
    // our initial load above and acquiring the lock.
    Doorbell* waiting = libk::atomic::load_acquire(&thread->waiting_doorbell);
    if (waiting == db) {
        libk::atomic::store_release(&thread->waiting_doorbell, nullptr);
    }
    unlock(db);
    local_irq_restore(irq);
    object::release(thread->waiting_lifetime);
    thread->waiting_lifetime = nullptr;
}

u64 doorbell_sleep_count() {
    return metric_total(&IpcMetrics::sleeps);
}
u64 doorbell_wake_count() {
    return metric_total(&IpcMetrics::wakes);
}
u64 doorbell_wait_calls() {
    return metric_total(&IpcMetrics::waits);
}
u64 doorbell_wait_immediate() {
    return metric_total(&IpcMetrics::immediate);
}
u64 notify_calls() {
    return metric_total(&IpcMetrics::notifies);
}
u64 notify_no_waiter() {
    return metric_total(&IpcMetrics::no_waiter);
}
u64 notify_woke_thread() {
    return metric_total(&IpcMetrics::wakes);
}
u64 doorbell_local_wake_count() {
    return metric_total(&IpcMetrics::local);
}
u64 doorbell_remote_wake_count() {
    return metric_total(&IpcMetrics::remote);
}

} // namespace ipc

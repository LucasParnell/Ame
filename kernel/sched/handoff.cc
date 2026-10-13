// kernel/sched/handoff.cc — synchronous handoff, reply tokens, and the IPC
// drain quantum.
//
// Owns the scheduler-side handoff plumbing: the per-CPU handoff_next slot, the
// sender/receiver reply link (Thread::reply_target), non-transitive priority
// donation (Thread::donated_key / has_donation), and the bounded IPC drain
// window that decays doorbell wake credit.

#include "anko/sched.h"
#include "sched_internal.h"

namespace sched {

// ---------------------------------------------------------------------------
// handoff_next — installs receiver as next thread for handoff
// ---------------------------------------------------------------------------
void set_handoff_next(Thread* receiver) {
    Eki& e         = current_eki();
    e.handoff_next = receiver;
}

// Consume a pending handoff target: post_syscall runs it instead of pick_next.
Thread* handoff_take_next(Eki& e) {
    Thread* next   = e.handoff_next;
    e.handoff_next = nullptr;
    return next;
}

// Drop a queued handoff target if it is the thread being retired.
void handoff_forget(Eki& e, Thread* t) {
    if (e.handoff_next == t) e.handoff_next = nullptr;
}

// True once a granted handoff quantum has outlived its deadline.
bool handoff_deadline_expired(const Thread* t, u64 now) {
    return t->handoff_deadline_ns != 0 && now >= t->handoff_deadline_ns;
}

// ---------------------------------------------------------------------------
// IPC drain quantum — bounded credit window for a doorbell-woken IPC server.
// Armed on the receiver's first run after a doorbell wake; expires once the
// thread stays CPU-bound past the window, stripping remaining wake credit.
// ---------------------------------------------------------------------------
void ipc_drain_quantum_begin(Thread* t, u64 now) {
    if (t->last_wake_reason == WAKE_DOORBELL
        && t->latency_hint == LATENCY_IPC_SERVER
        && !t->in_drain_quantum) {
        t->in_drain_quantum       = true;
        t->drain_quantum_start_ns = now;
    }
}

void ipc_drain_quantum_expire(Thread* t, u64 now) {
    if (handoff_deadline_expired(t, now)) {
        t->handoff_deadline_ns = 0;
        t->has_donation = false;
        t->donated_key = 0;
    }
    if (t->in_drain_quantum && now - t->drain_quantum_start_ns > kIpcDrainQuantumNs) {
        t->in_drain_quantum = false;
        t->wake_credit_ns   = 0;
    }
}

// ---------------------------------------------------------------------------
// Reply tokens — release a cross-linked sender/receiver pair when one side
// dies before the handoff completes.
// ---------------------------------------------------------------------------
void handoff_abort_reply(Thread* t) {
    if (!t->reply_target) return;
    Thread* peer = t->reply_target;
    t->reply_target = nullptr;
    if (peer->reply_target == t) {
        peer->reply_target = nullptr;
        peer->tf.x[0] = static_cast<u64>(E_DEAD);
        wake(peer);
    }
}

// ---------------------------------------------------------------------------
// Non-transitive priority donation — set by notify_handoff, cleared by
// reply_handoff.  effective_key() consults the donation as a lower-bound key.
// ---------------------------------------------------------------------------
void handoff_grant_donation(Thread* receiver, const Thread* sender) {
    receiver->donated_key  = sender->vruntime_ns;
    receiver->has_donation = true;
}

void handoff_clear_donation(Thread* donor) {
    donor->has_donation = false;
    donor->donated_key  = 0;
}

} // namespace sched

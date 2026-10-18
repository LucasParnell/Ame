#pragma once
// Scheduler-internal interfaces shared between the scheduler translation units.
// Not public Anko API (that is anko/sched.h). Symbols have external linkage only
// so the scheduler can be compiled as several modules.

#include "anko/sched.h"

namespace sched {

inline constexpr u64 kIpcDrainQuantumNs = 100000ULL; // 100 us (IPC drain quantum)
inline constexpr u32 kMaxPrincipalsLocal = kMaxPrincipals; // alias for clarity

// --- Band / class policy (sched_class.cc) ----------------------------------
u32 band_index(SchedBand band);
u32 band_rank(SchedBand band);
bool is_vm_band(SchedBand band);
u64 wake_credit_cap(const Thread* t);
u64 wake_credit_for(const Thread* t, WakeReason reason, u32 hints);
u64 class_target_latency_ns(SchedClass cls);
u64 class_min_gran_ns(SchedClass cls);
u64 class_max_slice_ns(SchedClass cls);
u64 wakeup_preempt_margin_ns(SchedClass cls);
u64 effective_key(const Thread* t);
u64 compute_slice_ns(const Thread* t, u32 runnable_in_principal);

// --- Fair-queue data structures (fair_queue.cc) ----------------------------
u32 ftq_sift_up(FairThreadQueue& q, u32 i);
void ftq_sift_down(FairThreadQueue& q, u32 i);
bool ftq_push(FairThreadQueue& q, Thread* t);
Thread* ftq_pop_min(FairThreadQueue& q);
Thread* ftq_peek_min(const FairThreadQueue& q);
bool ftq_remove(FairThreadQueue& q, Thread* t);
u64 pe_key(const FairPrincipalQueue& q, const PrincipalEntity* pe);
void fpq_sift_up(FairPrincipalQueue& q, u32 i);
void fpq_sift_down(FairPrincipalQueue& q, u32 i);
bool fpq_push(FairPrincipalQueue& q, PrincipalEntity* pe);
void fpq_rebuild(FairPrincipalQueue& q);
PrincipalEntity* fpq_pop_min(FairPrincipalQueue& q);
PrincipalEntity* fpq_peek_min(const FairPrincipalQueue& q);
void fpq_resift(FairPrincipalQueue& q, PrincipalEntity* pe);
bool fpq_remove(FairPrincipalQueue& q, PrincipalEntity* pe);
FairPrincipalQueue& band_for(Eki& e, SchedBand band);
BandQueue& band_threads(Eki& e, const PrincipalEntity& pe, SchedBand band);

// --- Per-CPU stations and core queues (runqueue.cc) ------------------------
Eki& eki_for_cpu(u32 cpu_id);
Eki& current_eki();
u64 online_mask();
u32 first_allowed_cpu(u64 allowed);
// Returns true only for an empty-to-nonempty publication needing an IPI.
bool pending_push(Eki& target, Thread* t);
void publish_queue_load(Eki& e);
bool dequeue_from_principal(Eki& e, Thread* t);

// --- Principal accounting / budgets / throttle (accounting.cc) -------------
bool principal_allocated(u32 pid);
bool principal_throttled(u32 pid);
// Atomic generation/weight publication; eligibility uses an atomic budget window.
// Budget queries require a live thread/process/context holding this principal.
struct PrincipalDispatch {
    u32 generation;
    u32 weight;
    bool available;
};
PrincipalDispatch principal_dispatch(u32 pid);
u32  principal_weight(u32 pid);
u32  principal_generation(u32 pid);
bool retain_principal(u32 pid);
void principal_init_bootstrap();
void principal_mirror_weights(u32 cpu_id);
void account_current(Eki& e);
void account_current_at(Eki& e, u64 now);
void refill_principal_budgets();
u64  fairness_max_lag();
u64  principal_runtime(u32 pid);
u64  principal_class_runtime(PrincipalClass class_);
u32  allocate_principal(PrincipalClass class_, u32 weight, u64 budget_ns, u64 period_ns);
void release_principal(u32 principal_id);

// --- SchedContext budgets (sched_context.cc) -------------------------------
void refill_sched_context_budgets();
bool sched_context_available(SchedContext* ctx);
bool charge_sched_context(SchedContext* ctx, u64 delta, u64 now);
bool thread_budget_available(const Thread* t);

// --- Handoff / reply / donation / IPC drain quantum (handoff.cc) -----------
Thread* handoff_take_next(Eki& e);
void handoff_forget(Eki& e, Thread* t);
bool handoff_deadline_expired(const Thread* t, u64 now);
void ipc_drain_quantum_begin(Thread* t, u64 now);
void ipc_drain_quantum_expire(Thread* t, u64 now);
void handoff_abort_reply(Thread* t);
void handoff_grant_donation(Thread* receiver, const Thread* sender);
void handoff_clear_donation(Thread* donor);

// --- Work stealing (steal.cc) ----------------------------------------------
void handle_pending_steal_requests(Eki& victim);
void maybe_send_steal_request(Eki& thief);

// --- Metrics (metrics.cc) --------------------------------------------------
void note_yield();

} // namespace sched

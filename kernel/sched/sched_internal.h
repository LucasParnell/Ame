#pragma once
// Scheduler-internal interfaces shared between the scheduler translation units.
// Not public Anko API (that is anko/sched.h). Symbols have external linkage only
// so the scheduler can be compiled as several modules.

#include "anko/sched.h"

namespace sched {

inline constexpr u64 kIpcDrainQuantumNs = 100000ULL; // 100 us (IPC drain quantum)

// --- Band / class policy (sched_class.cc) ----------------------------------
u32 band_index(SchedBand band);
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
FairPrincipalQueue& band_for(Eki& e, SchedBand band);
BandQueue& band_threads(PrincipalEntity& pe, SchedBand band);
u64& min_vruntime_for(Eki& e, SchedBand band);

} // namespace sched

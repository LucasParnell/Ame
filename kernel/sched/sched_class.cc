// kernel/sched/sched_class.cc — scheduling class and band policy.
//
// Band/class tables, wake credit, the fair-queue pick key, and slice sizing.
// Pure functions over anko/sched.h types: no global or per-CPU state.

#include "anko/sched.h"
#include "sched_internal.h"

namespace sched {

// ---------------------------------------------------------------------------
// Band helpers
// ---------------------------------------------------------------------------
u32 band_index(SchedBand band) {
    const u32 i = static_cast<u32>(band);
    return (i < NUM_BANDS) ? i : static_cast<u32>(SCHED_BAND_FAIR);
}

u32 band_rank(SchedBand band) {
    // Enum values are ABI identifiers, not dispatch precedence.
    switch (band) {
    case SCHED_BAND_RT_NATIVE: return 0;
    case SCHED_BAND_LATENCY: return 1;
    case SCHED_BAND_VM_LATENCY: return 2;
    case SCHED_BAND_FAIR: return 3;
    case SCHED_BAND_VM_FAIR: return 4;
    case SCHED_BAND_BATCH: return 5;
    default: return 6;
    }
}

bool is_vm_band(SchedBand band) {
    return band == SCHED_BAND_VM_LATENCY || band == SCHED_BAND_VM_FAIR;
}

u64 wake_credit_cap(const Thread* t) {
    if (!t) return 0;
    switch (t->sched.sched_class) {
    case SCHED_BAND_LATENCY:
        return (t->latency_hint == LATENCY_IPC_SERVER) ? 200000ULL : 300000ULL;
    case SCHED_BAND_FAIR:
        return 75000ULL;
    case SCHED_BAND_VM_LATENCY:
        return 150000ULL;
    default:
        return 0ULL;
    }
}

u64 wake_credit_for(const Thread* t, WakeReason reason, u32 hints) {
    if (!t) return 0;
    const SchedBand band = t->sched.sched_class;
    if (band == SCHED_BAND_RT_NATIVE || band == SCHED_BAND_BATCH ||
        band == SCHED_BAND_VM_FAIR || band == SCHED_BAND_IDLE) {
        return 0;
    }

    if (band == SCHED_BAND_VM_LATENCY) {
        // VM latency credit is only for host-visible events: injected IRQs or
        // explicitly latency-tagged virtio/doorbell kicks.  VM_FAIR gets none.
        if (reason == WAKE_IRQ || (hints & IPC_HINT_LATENCY)) return wake_credit_cap(t);
        return 0;
    }

    if (band == SCHED_BAND_FAIR && !(hints & IPC_HINT_LATENCY)) return 0;
    if (band == SCHED_BAND_LATENCY &&
        !(hints & IPC_HINT_LATENCY) && t->latency_hint != LATENCY_IPC_SERVER) return 0;
    return wake_credit_cap(t);
}

// Default per-band target latency / min granularity / max slice (all ns)
u64 class_target_latency_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_BAND_LATENCY:    return  4000000ULL; //  4 ms
    case SCHED_BAND_VM_LATENCY: return  6000000ULL; //  6 ms
    case SCHED_BAND_FAIR:       return 12000000ULL; // 12 ms
    case SCHED_BAND_VM_FAIR:    return 12000000ULL; // 12 ms
    case SCHED_BAND_BATCH:      return 24000000ULL; // 24 ms
    case SCHED_BAND_IDLE:       return 50000000ULL; // 50 ms
    default:                    return 12000000ULL;
    }
}
u64 class_min_gran_ns(SchedClass cls) {
    (void)cls;
    return 500000ULL; // 0.5 ms for all classes
}
u64 class_max_slice_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_BAND_VM_LATENCY: return 6000000ULL;
    case SCHED_BAND_BATCH:      return 12000000ULL;
    default:                    return 8000000ULL;
    }
}

u64 wakeup_preempt_margin_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_BAND_LATENCY:    return  50000ULL; //  50 us
    case SCHED_BAND_VM_LATENCY: return 100000ULL; // 100 us
    case SCHED_BAND_FAIR:       return 250000ULL; // 250 us
    default:                    return 0ULL;
    }
}


// ---------------------------------------------------------------------------
// Effective pick key for fair queue ordering
// ---------------------------------------------------------------------------
u64 effective_key(const Thread* t) {
    const i64 cap    = static_cast<i64>(wake_credit_cap(t));
    const i64 credit = t->wake_credit_ns < cap ? t->wake_credit_ns : cap;
    const i64 key    = static_cast<i64>(t->vruntime_ns) - credit;
    u64 k = static_cast<u64>(key < 0 ? 0 : key);
    // Non-transitive priority donation from handoff sender
    if (t->has_donation && t->donated_key < k) k = t->donated_key;
    return k;
}

// ---------------------------------------------------------------------------
// Slice calculation
// ---------------------------------------------------------------------------
u64 compute_slice_ns(const Thread* t, u32 runnable_in_principal) {
    const u64 target = class_target_latency_ns(t->sched.sched_class);
    const u32 n      = runnable_in_principal > 0 ? runnable_in_principal : 1u;
    // Proportional to weight among peers
    u64 slice = target * t->sched.weight / (static_cast<u64>(n) * NICE_0_WEIGHT);
    if (slice < class_min_gran_ns(t->sched.sched_class))
        slice = class_min_gran_ns(t->sched.sched_class);
    if (slice > class_max_slice_ns(t->sched.sched_class))
        slice = class_max_slice_ns(t->sched.sched_class);
    return slice;
}


} // namespace sched

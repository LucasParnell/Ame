// kernel/sched/fair_queue.cc — fair-queue data structures.
//
// FairThreadQueue and FairPrincipalQueue min-heaps plus band routing. Pure
// data-structure code over anko/sched.h types: no global or per-CPU state.

#include "anko/sched.h"
#include "sched_internal.h"

namespace sched {

// ---------------------------------------------------------------------------
// FairThreadQueue min-heap (keyed by effective_key)
// ---------------------------------------------------------------------------
u32 ftq_sift_up(FairThreadQueue& q, u32 i) {
    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (effective_key(q.data[parent]) <= effective_key(q.data[i])) break;
        Thread* tmp              = q.data[parent];
        q.data[parent]           = q.data[i];
        q.data[i]                = tmp;
        q.data[parent]->heap_idx = parent;
        q.data[i]->heap_idx      = i;
        i = parent;
    }
    return i;
}

void ftq_sift_down(FairThreadQueue& q, u32 i) {
    while (true) {
        u32 left  = 2 * i + 1;
        u32 right = 2 * i + 2;
        u32 smallest = i;
        if (left  < q.size && effective_key(q.data[left])  < effective_key(q.data[smallest]))
            smallest = left;
        if (right < q.size && effective_key(q.data[right]) < effective_key(q.data[smallest]))
            smallest = right;
        if (smallest == i) break;
        Thread* tmp              = q.data[i];
        q.data[i]                = q.data[smallest];
        q.data[smallest]         = tmp;
        q.data[i]->heap_idx      = i;
        q.data[smallest]->heap_idx = smallest;
        i = smallest;
    }
}

bool ftq_push(FairThreadQueue& q, Thread* t) {
    if (q.size >= kFairQueueCap) return false;
    q.data[q.size] = t;
    t->heap_idx    = q.size;
    ++q.size;
    (void)ftq_sift_up(q, q.size - 1);
    return true;
}

Thread* ftq_pop_min(FairThreadQueue& q) {
    if (q.size == 0) return nullptr;
    Thread* min = q.data[0];
    --q.size;
    if (q.size > 0) {
        q.data[0] = q.data[q.size];
        q.data[0]->heap_idx = 0;
        ftq_sift_down(q, 0);
    }
    min->heap_idx = 0;
    min->on_runqueue = false;
    return min;
}

Thread* ftq_peek_min(const FairThreadQueue& q) {
    return q.size ? q.data[0] : nullptr;
}

// Remove thread at known heap_idx — used by work stealing.
bool ftq_remove(FairThreadQueue& q, Thread* t) {
    if (q.size == 0 || !t) return false;
    u32 i = t->heap_idx;
    if (i >= q.size || q.data[i] != t) {
        // Linear scan fallback for stale heap_idx after lazy repairs.
        for (i = 0; i < q.size; ++i) {
            if (q.data[i] == t) break;
        }
        if (i >= q.size) return false;
    }
    --q.size;
    if (i < q.size) {
        q.data[i] = q.data[q.size];
        q.data[i]->heap_idx = i;
        const u32 new_i = ftq_sift_up(q, i);
        ftq_sift_down(q, new_i);
    }
    t->heap_idx = 0;
    t->on_runqueue = false;
    return true;
}

// ---------------------------------------------------------------------------
// FairPrincipalQueue min-heap (keyed by per-band vruntime)
// ---------------------------------------------------------------------------
u64 pe_key(const FairPrincipalQueue& q, const PrincipalEntity* pe) {
    return pe ? pe->vruntime[band_index(q.band)] : ~0ull;
}

void fpq_sift_up(FairPrincipalQueue& q, u32 i) {
    const u32 bi = band_index(q.band);
    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (pe_key(q, q.data[parent]) <= pe_key(q, q.data[i])) break;
        PrincipalEntity* tmp = q.data[parent];
        q.data[parent] = q.data[i];
        q.data[i]      = tmp;
        q.data[parent]->heap_idx[bi] = static_cast<u8>(parent);
        q.data[i]->heap_idx[bi]      = static_cast<u8>(i);
        i = parent;
    }
}

void fpq_sift_down(FairPrincipalQueue& q, u32 i) {
    const u32 bi = band_index(q.band);
    while (true) {
        u32 left     = 2 * i + 1;
        u32 right    = 2 * i + 2;
        u32 smallest = i;
        if (left  < q.size && pe_key(q, q.data[left])  < pe_key(q, q.data[smallest])) smallest = left;
        if (right < q.size && pe_key(q, q.data[right]) < pe_key(q, q.data[smallest])) smallest = right;
        if (smallest == i) break;
        PrincipalEntity* tmp = q.data[i];
        q.data[i]            = q.data[smallest];
        q.data[smallest]     = tmp;
        q.data[i]->heap_idx[bi] = static_cast<u8>(i);
        q.data[smallest]->heap_idx[bi] = static_cast<u8>(smallest);
        i = smallest;
    }
}

bool fpq_push(FairPrincipalQueue& q, PrincipalEntity* pe) {
    if (q.size >= kMaxPrincipals) return false;
    const u32 bi = band_index(q.band);
    q.data[q.size] = pe;
    pe->heap_idx[bi] = static_cast<u8>(q.size);
    ++q.size;
    fpq_sift_up(q, q.size - 1);
    return true;
}

void fpq_rebuild(FairPrincipalQueue& q) {
    const u32 bi = band_index(q.band);
    u32 n = q.size;
    if (n > kMaxPrincipals) n = kMaxPrincipals;

    u32 w = 0;
    for (u32 r = 0; r < n; ++r) {
        if (q.data[r]) q.data[w++] = q.data[r];
    }
    for (u32 r = w; r < kMaxPrincipals; ++r) q.data[r] = nullptr;

    q.size = w;
    if (w == 0) return;

    for (u32 i = w / 2; i > 0; --i) {
        fpq_sift_down(q, i - 1);
    }
    for (u32 i = 0; i < w; ++i) {
        if (q.data[i]) q.data[i]->heap_idx[bi] = static_cast<u8>(i);
    }
}

PrincipalEntity* fpq_pop_min(FairPrincipalQueue& q) {
    if (q.size == 0) return nullptr;
    const u32 bi = band_index(q.band);

    if (q.size > kMaxPrincipals || q.data[0] == nullptr) {
        fpq_rebuild(q);
        if (q.size == 0) return nullptr;
    }

    const u32 old_size = q.size;
    PrincipalEntity* min = q.data[0];

    const u32 new_size = old_size - 1;
    q.size = new_size;

    if (new_size > 0) {
        PrincipalEntity* moved = q.data[new_size];
        q.data[new_size] = nullptr;
        q.data[0] = moved;

        if (moved) {
            moved->heap_idx[bi] = 0;
            fpq_sift_down(q, 0);
        } else {
            fpq_rebuild(q);
        }
    } else {
        q.data[0] = nullptr;
    }

    if (min) min->heap_idx[bi] = 0xff;
    return min;
}

PrincipalEntity* fpq_peek_min(const FairPrincipalQueue& q) {
    return q.size ? q.data[0] : nullptr;
}

void fpq_resift(FairPrincipalQueue& q, PrincipalEntity* pe) {
    if (!pe) return;
    const u32 bi = band_index(q.band);
    if (!pe->in_queue[bi]) return;
    const u32 idx = pe->heap_idx[bi];
    if (idx >= q.size || q.data[idx] != pe) {
        fpq_rebuild(q);
        return;
    }
    fpq_sift_up(q, idx);
    const u32 after_up = pe->heap_idx[bi];
    if (after_up < q.size) fpq_sift_down(q, after_up);
}

// ---------------------------------------------------------------------------
// Band routing.  The principal queue and per-principal thread heap must be
// selected by the same band; otherwise a latency-selected principal can pop a
// fair or VM thread from a shared heap.
// ---------------------------------------------------------------------------
FairPrincipalQueue& band_for(Eki& e, SchedBand band) {
    switch (band) {
    case SCHED_BAND_LATENCY:    return e.latency_principals;
    case SCHED_BAND_VM_LATENCY: return e.vm_latency_principals;
    case SCHED_BAND_FAIR:       return e.fair_principals;
    case SCHED_BAND_VM_FAIR:    return e.vm_fair_principals;
    case SCHED_BAND_BATCH:      return e.batch_principals;
    case SCHED_BAND_IDLE:       return e.idle_principals;
    default:                    return e.fair_principals;
    }
}

BandQueue& band_threads(PrincipalEntity& pe, SchedBand band) {
    switch (band) {
    case SCHED_BAND_RT_NATIVE:  return pe.rt;
    case SCHED_BAND_LATENCY:    return pe.latency;
    case SCHED_BAND_FAIR:       return pe.fair;
    case SCHED_BAND_VM_LATENCY: return pe.vm_latency;
    case SCHED_BAND_VM_FAIR:    return pe.vm_fair;
    case SCHED_BAND_BATCH:      return pe.batch;
    case SCHED_BAND_IDLE:       return pe.idle;
    default:                    return pe.fair;
    }
}

u64& min_vruntime_for(Eki& e, SchedBand band) {
    return e.min_vruntime[band_index(band)];
}


} // namespace sched

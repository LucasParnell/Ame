// Ressha queue policy and index adapters. Bounded heap mechanics live in libk;
// scheduler membership and band selection remain here.
#include "anko/sched.h"
#include "sched_internal.h"
#include "libk/indexed_heap.h"

namespace sched {
namespace {
void thread_position(Thread* t, u32 index) { t->heap_idx = index; }
auto principal_key(const FairPrincipalQueue& q) {
    return [&q](const PrincipalEntity* pe) { return pe_key(q, pe); };
}
auto principal_position(const FairPrincipalQueue& q) {
    return [bi = band_index(q.band)](PrincipalEntity* pe, u32 index) {
        pe->heap_idx[bi] = index == libk::indexed_heap::no_index ? 0xff : static_cast<u8>(index);
    };
}
}

u32 ftq_sift_up(FairThreadQueue& q, u32 i) {
    return libk::indexed_heap::sift_up(q.data, i, effective_key, thread_position);
}
void ftq_sift_down(FairThreadQueue& q, u32 i) {
    libk::indexed_heap::sift_down(q.data, q.size, i, effective_key, thread_position);
}
bool ftq_push(FairThreadQueue& q, Thread* t) {
    return t && libk::indexed_heap::push(q.data, q.size, t, effective_key, thread_position);
}
Thread* ftq_pop_min(FairThreadQueue& q) {
    if (!q.size) return nullptr;
    auto* t = q.data[0];
    if (!libk::indexed_heap::remove_at(q.data, q.size, 0, effective_key, thread_position)) return nullptr;
    t->on_runqueue = false;
    return t;
}
Thread* ftq_peek_min(const FairThreadQueue& q) { return q.size ? q.data[0] : nullptr; }
bool ftq_remove(FairThreadQueue& q, Thread* t) {
    if (!t || !q.size) return false;
    u32 i = t->heap_idx;
    if (i >= q.size || q.data[i] != t) {
        for (i = 0; i < q.size && q.data[i] != t; ++i) {}
        if (i == q.size) return false;
    }
    if (!libk::indexed_heap::remove_at(q.data, q.size, i, effective_key, thread_position)) return false;
    t->on_runqueue = false;
    return true;
}

u64 pe_key(const FairPrincipalQueue&, const PrincipalEntity* pe) {
    return pe ? pe->service_vruntime_ns : ~0ull;
}
void fpq_sift_up(FairPrincipalQueue& q, u32 i) {
    libk::indexed_heap::sift_up(q.data, i, principal_key(q), principal_position(q));
}
void fpq_sift_down(FairPrincipalQueue& q, u32 i) {
    libk::indexed_heap::sift_down(q.data, q.size, i, principal_key(q), principal_position(q));
}
bool fpq_push(FairPrincipalQueue& q, PrincipalEntity* pe) {
    return pe && libk::indexed_heap::push(q.data, q.size, pe, principal_key(q), principal_position(q));
}
void fpq_rebuild(FairPrincipalQueue& q) {
    u32 size = q.size < kMaxPrincipals ? q.size : kMaxPrincipals;
    u32 count = 0;
    for (u32 i = 0; i < size; ++i) if (q.data[i]) q.data[count++] = q.data[i];
    for (u32 i = count; i < kMaxPrincipals; ++i) q.data[i] = nullptr;
    q.size = count;
    for (u32 i = count / 2; i; --i) fpq_sift_down(q, i - 1);
    for (u32 i = 0; i < count; ++i) principal_position(q)(q.data[i], i);
}
PrincipalEntity* fpq_pop_min(FairPrincipalQueue& q) {
    if (q.size > kMaxPrincipals || (q.size && !q.data[0])) fpq_rebuild(q);
    if (!q.size) return nullptr;
    auto* pe = q.data[0];
    if (!libk::indexed_heap::remove_at(q.data, q.size, 0, principal_key(q), principal_position(q))) return nullptr;
    return pe;
}
PrincipalEntity* fpq_peek_min(const FairPrincipalQueue& q) { return q.size ? q.data[0] : nullptr; }
void fpq_resift(FairPrincipalQueue& q, PrincipalEntity* pe) {
    if (!pe || !pe->in_queue[band_index(q.band)]) return;
    const u32 i = pe->heap_idx[band_index(q.band)];
    if (i >= q.size || q.data[i] != pe) { fpq_rebuild(q); return; }
    fpq_sift_up(q, i);
    fpq_sift_down(q, pe->heap_idx[band_index(q.band)]);
}
bool fpq_remove(FairPrincipalQueue& q, PrincipalEntity* pe) {
    if (!pe || !q.size) return false;
    const u32 bi = band_index(q.band);
    u32 i = pe->heap_idx[bi];
    if (i >= q.size || q.data[i] != pe) {
        for (i = 0; i < q.size && q.data[i] != pe; ++i) {}
        if (i == q.size) return false;
    }
    if (!libk::indexed_heap::remove_at(q.data, q.size, i, principal_key(q), principal_position(q))) return false;
    pe->in_queue[bi] = false;
    return true;
}

FairPrincipalQueue& band_for(Eki& e, SchedBand band) {
    switch (band) {
    case SCHED_BAND_LATENCY: return e.latency_principals;
    case SCHED_BAND_VM_LATENCY: return e.vm_latency_principals;
    case SCHED_BAND_VM_FAIR: return e.vm_fair_principals;
    case SCHED_BAND_BATCH: return e.batch_principals;
    case SCHED_BAND_IDLE: return e.idle_principals;
    default: return e.fair_principals;
    }
}
BandQueue& band_threads(Eki& e, const PrincipalEntity& pe, SchedBand band) {
    const u32 index = band_index(band);
    return e.thread_heaps[pe.principal_id][index ? index - 1 : SCHED_BAND_FAIR - 1];
}
} // namespace sched

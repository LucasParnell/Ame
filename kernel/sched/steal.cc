// kernel/sched/steal.cc — cross-CPU work stealing.
//
// A CPU that runs dry asks the busiest peer for migration-safe work via a
// single pre-allocated CAS'd StealRequest.  Victims only hand over lower-band
// threads that opt into migration; latency/RT bands and pinned or IPC-hot
// threads are never stolen.

#include "anko/sched.h"
#include "anko/smp.h"
#include "libk/metric.h"
#include "sched_internal.h"

namespace sched {

using libk::metric_inc;

static bool is_stealable(const Thread* t) {
    if (!t || __atomic_load_n(&t->terminate_requested, __ATOMIC_ACQUIRE)) return false;
    if (!t->on_runqueue || t->state != TS_RUNNABLE_QUEUED) return false;
    const SchedBand band = t->sched.sched_class;
    if (band == SCHED_BAND_RT_NATIVE || band == SCHED_BAND_LATENCY ||
        band == SCHED_BAND_VM_LATENCY) return false;
    if (t->sched_context && (t->sched_context->flags & SCHED_CTX_NO_STEAL)) return false;
    if (is_vm_band(band) &&
        (!t->sched_context || !(t->sched_context->flags & SCHED_CTX_STEALABLE))) return false;
    // Hard affinity: pinned to one CPU
    if (t->affinity_mask != 0 && __builtin_popcountll(t->affinity_mask) == 1) return false;
    if (t->ipc_hot_until_ns > sched_clock_ns()) return false;
    return band == SCHED_BAND_FAIR || band == SCHED_BAND_BATCH || band == SCHED_BAND_VM_FAIR;
}

static Thread* select_stealable_from_band(FairPrincipalQueue& q) {
    const SchedBand sb = q.band;
    for (u32 i = 0; i < q.size; ++i) {
        PrincipalEntity* pe = q.data[i];
        if (!pe) continue;
        BandQueue& threads = band_threads(*pe, sb);
        for (u32 j = 0; j < threads.size; ++j) {
            if (is_stealable(threads.data[j])) return threads.data[j];
        }
    }
    return nullptr;
}

static Thread* select_stealable(Eki& victim) {
    // Steal only lower-priority, migration-safe work.  VM fair vCPUs are
    // migration-exempt by default unless SCHED_CTX_STEALABLE is set.
    if (Thread* t = select_stealable_from_band(victim.fair_principals)) return t;
    if (Thread* t = select_stealable_from_band(victim.vm_fair_principals)) return t;
    if (Thread* t = select_stealable_from_band(victim.batch_principals)) return t;
    return nullptr;
}

void handle_pending_steal_requests(Eki& victim) {
    StealRequest* list = __atomic_exchange_n(
        &victim.pending_steal_requests.head, nullptr, __ATOMIC_ACQ_REL);
    while (list) {
        StealRequest* nxt = list->next;
        metric_inc(victim.steal_requests_received);

        Thread* t = select_stealable(victim);
        if (!t) {
            metric_inc(victim.steals_denied);
        } else {
            if (!dequeue_from_principal(victim, t)) {
                metric_inc(victim.steals_denied);
                list = nxt;
                continue;
            }
            t->state = TS_RUNNABLE_PENDING_REMOTE;

            const u32 thief_cpu = list->thief_cpu;
            t->cpu_id = thief_cpu;
            pending_push(eki_for_cpu(thief_cpu), t);
            smp::send_wake_ipi(thief_cpu);
            metric_inc(victim.steals_granted);

            // Free request back to thief's eki
            if (thief_cpu < kMaxCpus)
                __atomic_store_n(&eki_for_cpu(thief_cpu).steal_req_in_flight, false, __ATOMIC_RELEASE);
        }

        if (!t) {
            // Still need to free the request
            if (list->thief_cpu < kMaxCpus)
                __atomic_store_n(&eki_for_cpu(list->thief_cpu).steal_req_in_flight, false, __ATOMIC_RELEASE);
        }
        list = nxt;
    }
}

void maybe_send_steal_request(Eki& thief) {
    const u32 on = online_cpu_count();
    if (on <= 1) return;

    // Don't send if already in flight
    if (__atomic_load_n(&thief.steal_req_in_flight, __ATOMIC_ACQUIRE)) return;

    // Find most loaded peer
    u32 victim_cpu = kInvalidCpu;
    u32 max_load   = 1; // only steal if victim has > 1 stealable thread
    const u32 n    = on < kMaxCpus ? on : kMaxCpus;
    for (u32 c = 0; c < n; ++c) {
        if (c == thief.cpu_id) continue;
        Eki& peer = eki_for_cpu(c);
        u32 load = peer.fair_principals.size + peer.vm_fair_principals.size + peer.batch_principals.size;
        if (load > max_load) { max_load = load; victim_cpu = c; }
    }
    if (victim_cpu == kInvalidCpu) return;

    __atomic_store_n(&thief.steal_req_in_flight, true, __ATOMIC_RELEASE);
    thief.steal_req_buf.thief_cpu = thief.cpu_id;
    thief.steal_req_buf.next      = nullptr;

    Eki& victim = eki_for_cpu(victim_cpu);
    StealRequest* old;
    do {
        old = __atomic_load_n(&victim.pending_steal_requests.head, __ATOMIC_ACQUIRE);
        thief.steal_req_buf.next = old;
    } while (!__atomic_compare_exchange_n(&victim.pending_steal_requests.head,
                                          &old, &thief.steal_req_buf,
                                          false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    metric_inc(thief.steal_requests_sent);
    smp::send_wake_ipi(victim_cpu);
}

} // namespace sched

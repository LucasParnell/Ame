// kernel/sched/runqueue.cc — Ressha/Eki V2 per-CPU fair scheduler.
//
// Pick order: native RT → native latency → VM latency → native fair →
//             VM fair → batch → idle.
// Fairness is hierarchical: pick principal (per-band min vruntime), then pick
// a thread/vCPU runner from that same band-specific principal queue.

#include "anko/sched.h"
#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/smp.h"
#include "anko/log.h"
#include "libk/metric.h"
#include "libk/printf.h"
#include "libk/string.h"

namespace sched {

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static Eki   g_eki[kMaxCpus]{};
static u32   g_online_cpu_count = 1;
static u64   g_yield_count = 0;
static u64   g_cntfrq = 0;          // cached at init

constexpr u32 kMaxPrincipalsLocal = kMaxPrincipals; // alias for clarity
static PrincipalSchedState g_principal_sched[kMaxPrincipalsLocal]{};
static u32 g_next_principal_id = 1; // principal 0 is reserved for the kernel/bootstrap bucket

constexpr u32 kMaxSchedContexts = 128;
static SchedContext g_sched_contexts[kMaxSchedContexts]{};
static bool g_sched_context_allocated[kMaxSchedContexts]{};
static u32 g_next_sched_context_id = 1;

static void pending_push(Eki& target, Thread* t);
static bool bind_thread_to_context(Thread* t, SchedContext* ctx);

using libk::metric_inc;
using libk::metric_load;

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
u64 sched_clock_ns() {
    u64 cnt;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(cnt) :: "memory");
    if (!g_cntfrq) return cnt; // before init; return ticks
    // Avoid 128-bit division (no __udivti3 in freestanding runtime):
    // split into whole-second and sub-second parts, both fit in u64.
    // rem < g_cntfrq ≤ 1 GHz, so rem * 1e9 < 2^60 — no overflow.
    const u64 sec = cnt / g_cntfrq;
    const u64 rem = cnt % g_cntfrq;
    return sec * 1000000000ULL + rem * 1000000000ULL / g_cntfrq;
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
static inline void send_event()          { asm volatile("sev" ::: "memory"); }
static inline void irq_enable()          { asm volatile("msr DAIFClr, #2" ::: "memory"); }
static inline void irq_disable()         { asm volatile("msr DAIFSet, #2" ::: "memory"); }

// Only enable IRQs for the actual WFI window.  Scheduler queue mutation must
// resume with IRQs masked, otherwise timer/IPI paths can reenter queue code.
static inline void wait_for_interrupt()  {
    irq_enable();
    asm volatile("wfi" ::: "memory");
    irq_disable();
}

static Eki& eki_for_cpu(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
    return g_eki[cpu_id];
}
static Eki& current_eki() { return eki_for_cpu(current_cpu_id()); }

static u64 online_mask() {
    const u32 n = g_online_cpu_count > kMaxCpus ? kMaxCpus : g_online_cpu_count;
    u64 m = 0;
    for (u32 i = 0; i < n; ++i) m |= (1ull << i);
    return m ? m : 1ull;
}

// ---------------------------------------------------------------------------
// Band helpers
// ---------------------------------------------------------------------------
static u32 band_index(SchedBand band) {
    const u32 i = static_cast<u32>(band);
    return (i < NUM_BANDS) ? i : static_cast<u32>(SCHED_BAND_FAIR);
}

static bool is_vm_band(SchedBand band) {
    return band == SCHED_BAND_VM_LATENCY || band == SCHED_BAND_VM_FAIR;
}

static u64 wake_credit_cap(const Thread* t) {
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

static u64 wake_credit_for(const Thread* t, WakeReason reason, u32 hints) {
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
static u64 class_target_latency_ns(SchedClass cls) {
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
static u64 class_min_gran_ns(SchedClass cls) {
    (void)cls;
    return 500000ULL; // 0.5 ms for all classes
}
static u64 class_max_slice_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_BAND_VM_LATENCY: return 6000000ULL;
    case SCHED_BAND_BATCH:      return 12000000ULL;
    default:                    return 8000000ULL;
    }
}

static u64 wakeup_preempt_margin_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_BAND_LATENCY:    return  50000ULL; //  50 us
    case SCHED_BAND_VM_LATENCY: return 100000ULL; // 100 us
    case SCHED_BAND_FAIR:       return 250000ULL; // 250 us
    default:                    return 0ULL;
    }
}

constexpr u64 kIpcDrainQuantumNs = 100000ULL; // 100 us

// ---------------------------------------------------------------------------
// Effective pick key for fair queue ordering
// ---------------------------------------------------------------------------
static u64 effective_key(const Thread* t) {
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
static u64 compute_slice_ns(const Thread* t, u32 runnable_in_principal) {
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

// ---------------------------------------------------------------------------
// FairThreadQueue min-heap (keyed by effective_key)
// ---------------------------------------------------------------------------
static u32 ftq_sift_up(FairThreadQueue& q, u32 i) {
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

static void ftq_sift_down(FairThreadQueue& q, u32 i) {
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

static bool ftq_push(FairThreadQueue& q, Thread* t) {
    if (q.size >= kFairQueueCap) return false;
    q.data[q.size] = t;
    t->heap_idx    = q.size;
    ++q.size;
    (void)ftq_sift_up(q, q.size - 1);
    return true;
}

static Thread* ftq_pop_min(FairThreadQueue& q) {
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

[[maybe_unused]] static Thread* ftq_peek_min(const FairThreadQueue& q) {
    return q.size ? q.data[0] : nullptr;
}

// Remove thread at known heap_idx — used by work stealing.
static bool ftq_remove(FairThreadQueue& q, Thread* t) {
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
static u64 pe_key(const FairPrincipalQueue& q, const PrincipalEntity* pe) {
    return pe ? pe->vruntime[band_index(q.band)] : ~0ull;
}

static void fpq_sift_up(FairPrincipalQueue& q, u32 i) {
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

static void fpq_sift_down(FairPrincipalQueue& q, u32 i) {
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

static bool fpq_push(FairPrincipalQueue& q, PrincipalEntity* pe) {
    if (q.size >= kMaxPrincipals) return false;
    const u32 bi = band_index(q.band);
    q.data[q.size] = pe;
    pe->heap_idx[bi] = static_cast<u8>(q.size);
    ++q.size;
    fpq_sift_up(q, q.size - 1);
    return true;
}

static void fpq_rebuild(FairPrincipalQueue& q) {
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

static PrincipalEntity* fpq_pop_min(FairPrincipalQueue& q) {
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

static PrincipalEntity* fpq_peek_min(const FairPrincipalQueue& q) {
    return q.size ? q.data[0] : nullptr;
}

// ---------------------------------------------------------------------------
// Band routing.  The principal queue and per-principal thread heap must be
// selected by the same band; otherwise a latency-selected principal can pop a
// fair or VM thread from a shared heap.
// ---------------------------------------------------------------------------
static FairPrincipalQueue& band_for(Eki& e, SchedBand band) {
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

static BandQueue& band_threads(PrincipalEntity& pe, SchedBand band) {
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

static u64& min_vruntime_for(Eki& e, SchedBand band) {
    return e.min_vruntime[band_index(band)];
}

// ---------------------------------------------------------------------------
// enqueue_fair — add thread to its principal entity's band-specific fair queue.
// On first thread for this principal on this CPU, normalize vruntimes to
// min_vruntime so long-sleeping threads don't get unlimited catch-up.
// ---------------------------------------------------------------------------
static bool enqueue_fair(Eki& e, Thread* t) {
    if (!t || t->state == TS_DEAD) return false;
    if (t == e.current && t->state == TS_RUNNING) return false;

    // The physical queue membership bit is the important invariant.  A stale
    // duplicate fair-queue entry can make one process resume with another
    // process's saved ELR, so reject duplicate insertion aggressively.
    if (t->on_runqueue) return false;
    if (t->state != TS_RUNNABLE_QUEUED) return false;

    const u32 pid = t->principal_id;
    if (pid >= kMaxPrincipals) return false;

    const SchedBand sb = t->sched.sched_class;
    const u32 bi = band_index(sb);
    PrincipalEntity& pe      = e.principal_entities[pid];
    FairPrincipalQueue& band = band_for(e, sb);
    BandQueue& threads       = band_threads(pe, sb);
    u64& min_vr              = min_vruntime_for(e, sb);

    // Normalize: clamp back-pay from sleeping.
    if (t->vruntime_ns < min_vr) t->vruntime_ns = min_vr;
    if (pe.vruntime[bi] < min_vr) pe.vruntime[bi] = min_vr;

    const u32 global_weight = (pid < kMaxPrincipalsLocal && g_principal_sched[pid].weight)
        ? g_principal_sched[pid].weight : NICE_0_WEIGHT;
    if (pe.weight == 0) pe.weight = global_weight;
    t->slice_ns = compute_slice_ns(t, pe.runnable[bi] + 1);

    if (!ftq_push(threads, t)) {
        metric_inc(e.fair_queue_overflows);
        ANKO_LOGF_ERROR("kernel/sched",
                        "fair queue overflow cpu=%u principal=%u band=%u tid=%u",
                        e.cpu_id, pid, static_cast<u32>(sb), t->tid);
        // Hard diagnostic: do not leave a runnable thread off-queue.  Parking the
        // thread is safer than creating a duplicate/stale entry that can corrupt
        // another process's trapframe.
        t->on_runqueue = false;
        t->state = TS_BLOCKED;
        return false;
    }

    t->on_runqueue = true;
    t->cpu_id = e.cpu_id;
    ++pe.runnable[bi];

    if (!pe.in_queue[bi]) {
        if (fpq_push(band, &pe)) {
            pe.in_queue[bi] = true;
        } else {
            metric_inc(e.fair_queue_overflows);
            ANKO_LOGF_ERROR("kernel/sched",
                            "principal queue overflow cpu=%u principal=%u band=%u",
                            e.cpu_id, pid, static_cast<u32>(sb));
            (void)ftq_remove(threads, t);
            if (pe.runnable[bi] > 0) --pe.runnable[bi];
            t->on_runqueue = false;
            t->state = TS_BLOCKED;
            return false;
        }
    }

    if (Process* p = find_process(t->pid)) ++p->runnable_threads;
    return true;
}

// ---------------------------------------------------------------------------
// dequeue_from_principal — remove thread from its principal's band heap.
// Called by work stealing and destruction.
// ---------------------------------------------------------------------------
static bool dequeue_from_principal(Eki& e, Thread* t) {
    if (!t) return false;
    const u32 pid = t->principal_id;
    if (pid >= kMaxPrincipals) return false;

    const SchedBand sb = t->sched.sched_class;
    const u32 bi = band_index(sb);
    PrincipalEntity& pe = e.principal_entities[pid];
    BandQueue& threads = band_threads(pe, sb);
    if (!ftq_remove(threads, t)) return false;
    if (pe.runnable[bi] > 0) --pe.runnable[bi];

    // Principal queue removal is lazy in pick_fair.

    if (Process* p = find_process(t->pid)) {
        if (p->runnable_threads > 0) --p->runnable_threads;
    }
    return true;
}

// Public: remove a thread from its CPU's fair queue (safe to call before TS_DEAD).
void dequeue_thread(Thread* t) {
    if (!t) return;
    if (!t->on_runqueue && t->state != TS_RUNNABLE_PENDING_REMOTE) return;
    const u32 cpu = (t->cpu_id < kMaxCpus) ? t->cpu_id : 0;
    (void)dequeue_from_principal(g_eki[cpu], t);
    t->on_runqueue = false;
}

// ---------------------------------------------------------------------------
// RT queue helpers
// ---------------------------------------------------------------------------
static void enqueue_rt(Eki& e, Thread* t) {
    if (!t || t->on_runqueue || t->state != TS_RUNNABLE_QUEUED) return;
    const u8 prio = t->sched.base_priority < kMaxRtPrio
        ? t->sched.base_priority : static_cast<u8>(0);
    RunQueue& q = e.rt.queues[prio];
    t->next = nullptr;
    if (!q.head) { q.head = q.tail = t; }
    else          { q.tail->next = t; q.tail = t; }
    ++q.count;
    e.rt.bitmap |= (1u << prio);
    t->on_runqueue = true;
    t->cpu_id = e.cpu_id;
}

static Thread* pick_rt(Eki& e) {
    while (e.rt.bitmap) {
        const u32 prio = 31 - __builtin_clz(e.rt.bitmap);
        RunQueue& q    = e.rt.queues[prio];
        while (q.head) {
            Thread* t = q.head;
            q.head    = t->next;
            if (!q.head) { q.tail = nullptr; e.rt.bitmap &= ~(1u << prio); }
            if (q.count) --q.count;
            t->next = nullptr;
            const bool valid = (t->state == TS_RUNNABLE_QUEUED)
                            && (t->cpu_id == e.cpu_id)
                            && (t != e.current);
            t->on_runqueue = false;
            if (valid) return t;
        }
        e.rt.bitmap &= ~(1u << prio);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// pick_fair — pick min-vruntime principal, then min-effective-key thread from
// the same band.  Skips globally throttled principals lazily.
// ---------------------------------------------------------------------------
static Thread* pick_fair(Eki& e, FairPrincipalQueue& band) {
    const SchedBand sb = band.band;
    const u32 bi = band_index(sb);
    while (band.size > 0) {
        PrincipalEntity* pe = fpq_peek_min(band);
        if (!pe) break;

        const bool global_throttled =
            pe->principal_id < kMaxPrincipalsLocal &&
            __atomic_load_n(&g_principal_sched[pe->principal_id].throttled, __ATOMIC_RELAXED);
        if (pe->throttled || global_throttled) {
            pe->throttled = true;
            fpq_pop_min(band);
            pe->in_queue[bi] = false;
            continue;
        }

        BandQueue& threads = band_threads(*pe, sb);
        Thread* t = ftq_pop_min(threads);
        if (!t) {
            fpq_pop_min(band);
            pe->in_queue[bi] = false;
            pe->runnable[bi] = 0;
            continue;
        }

        if (pe->runnable[bi] > 0) --pe->runnable[bi];
        if (Process* p = find_process(t->pid)) {
            if (p->runnable_threads > 0) --p->runnable_threads;
        }

        fpq_pop_min(band);
        if (pe->runnable[bi] > 0) {
            (void)fpq_push(band, pe);
        } else {
            pe->in_queue[bi] = false;
        }

        u64& min_vr = min_vruntime_for(e, sb);
        if (pe->vruntime[bi] > min_vr) min_vr = pe->vruntime[bi];

        // Lazy stale-entry filter.  This is intentionally stricter than just
        // checking TS_DEAD: a stale entry for a running, blocked, pending, or
        // migrated thread must never be dispatched.
        if (t->state != TS_RUNNABLE_QUEUED) continue;
        if (t->cpu_id != e.cpu_id) continue;
        if (t == e.current) continue;
        return t;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Runtime accounting — charge elapsed time to thread / process / principal.
// Called before every context switch and on tick.
// ---------------------------------------------------------------------------
static void account_current(Eki& e) {
    Thread* t = e.current;
    if (!t || t == e.idle) return;
    if (t->state != TS_RUNNING) return;

    const u64 now   = sched_clock_ns();
    const u64 delta = now - t->last_start_ns;
    t->last_start_ns = now;

    t->runtime_ns += delta;
    if (Process* p = find_process(t->pid)) p->runtime_ns += delta;

    const u32 pid = t->principal_id;
    if (pid < kMaxPrincipalsLocal) {
        PrincipalSchedState& ps = g_principal_sched[pid];
        __atomic_add_fetch(&ps.runtime_ns,        delta, __ATOMIC_RELAXED);
        __atomic_add_fetch(&ps.used_in_period_ns, delta, __ATOMIC_RELAXED);

        // Per-CPU principal entity vruntime, tracked per band so a principal
        // cannot carry latency placement into fair/VM queues.
        PrincipalEntity& pe = e.principal_entities[pid];
        pe.runtime_ns   += delta;
        const u32 pw     = pe.weight ? pe.weight : NICE_0_WEIGHT;
        pe.vruntime[band_index(t->sched.sched_class)] += delta * NICE_0_WEIGHT / pw;

        // Thread vruntime
        const u32 tw     = t->sched.weight ? t->sched.weight : NICE_0_WEIGHT;
        t->vruntime_ns  += delta * NICE_0_WEIGHT / tw;

        // Bound explicit sched contexts independently from principal budgets.
        if (t->sched_context) {
            SchedContext* ctx = t->sched_context;
            ctx->used_ns += delta;
            if (ctx->budget_ns > 0 && ctx->used_ns >= ctx->budget_ns) {
                request_switch(false, SWITCH_BUDGET);
            }
        }

        // Decay IPC wake credit
        const u64 decay  = static_cast<u64>(t->wake_credit_ns) < delta
            ? static_cast<u64>(t->wake_credit_ns < 0 ? 0 : t->wake_credit_ns)
            : delta;
        t->wake_credit_ns -= static_cast<i64>(decay);
        if (t->wake_credit_ns < 0) t->wake_credit_ns = 0;

        // IPC drain quantum expiry: strip credit if went CPU-bound
        if (t->in_drain_quantum) {
            if (now - t->drain_quantum_start_ns > kIpcDrainQuantumNs) {
                t->in_drain_quantum = false;
                t->wake_credit_ns   = 0;
            }
        }

        // Budget enforcement
        if (ps.budget_ns > 0 && ps.used_in_period_ns >= ps.budget_ns) {
            ps.throttled = true;
            pe.throttled = true;
            request_switch(false, SWITCH_BUDGET);
        }
    }
}

// ---------------------------------------------------------------------------
// Period refill — called from tick() for each principal with a budget.
// ---------------------------------------------------------------------------
static void refill_principal_budgets() {
    const u64 now = sched_clock_ns();
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        PrincipalSchedState& ps = g_principal_sched[i];
        if (ps.budget_ns == 0) continue;
        if (ps.period_ns == 0 || now - ps.period_start_ns < ps.period_ns) continue;
        ps.used_in_period_ns = 0;
        ps.period_start_ns   = now;
        if (ps.throttled) {
            ps.throttled = false;
            for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
                if (i < kMaxPrincipals) g_eki[cpu].principal_entities[i].throttled = false;
            }
        }
    }
}

static void refill_sched_context_budgets() {
    const u64 now = sched_clock_ns();
    for (u32 i = 0; i < kMaxSchedContexts; ++i) {
        if (!g_sched_context_allocated[i]) continue;
        SchedContext& ctx = g_sched_contexts[i];
        if (ctx.budget_ns == 0 || ctx.period_ns == 0) continue;
        if (now - ctx.period_start_ns < ctx.period_ns) continue;
        ctx.used_ns = 0;
        ctx.period_start_ns = now;
    }
}

// ---------------------------------------------------------------------------
// Pending-wake drain — MPSC stack → FIFO reverse → enqueue_fair
// ---------------------------------------------------------------------------
static void drain_pending_wakes(Eki& e) {
    Thread* list = __atomic_exchange_n(&e.pending_wakes.head, nullptr, __ATOMIC_ACQ_REL);
    if (!list) return;
    metric_inc(e.pending_wake_drains);

    // Reverse LIFO stack → FIFO order
    Thread* rev = nullptr;
    while (list) {
        Thread* nxt     = list->remote_next;
        list->remote_next = rev;
        rev             = list;
        list            = nxt;
    }

    while (rev) {
        Thread* t   = rev;
        rev         = rev->remote_next;
        t->remote_next = nullptr;
        if (t->state == TS_DEAD) continue;
        if (t->state != TS_RUNNABLE_PENDING_REMOTE) continue;
        if (t == e.current) {
            // Current thread must remain running; do not orphan it in a
            // pending state if the wake list happens to contain it.
            t->state = TS_RUNNING;
            continue;
        }
        if (t->on_runqueue) continue;
        t->state = TS_RUNNABLE_QUEUED;
        metric_inc(e.wake_remote);
        (void)enqueue_fair(e, t);
    }
}

// ---------------------------------------------------------------------------
// Pending-wake push (lock-free CAS, called from remote CPUs)
// ---------------------------------------------------------------------------
static void pending_push(Eki& target, Thread* t) {
    Thread* old;
    do {
        old = __atomic_load_n(&target.pending_wakes.head, __ATOMIC_ACQUIRE);
        t->remote_next = old;
    } while (!__atomic_compare_exchange_n(&target.pending_wakes.head,
                                          &old, t, false,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

// ---------------------------------------------------------------------------
// CPU selection
// ---------------------------------------------------------------------------
static u32 first_allowed_cpu(u64 allowed) {
    for (u32 i = 0; i < kMaxCpus; ++i) {
        if (allowed & (1ull << i)) return i;
    }
    return 0;
}

static u32 least_loaded_cpu_for(const Thread* t) {
    const u64 allowed = t->affinity_mask ? (t->affinity_mask & online_mask()) : online_mask();
    u32 best_cpu  = first_allowed_cpu(allowed);
    u32 best_load = ~0u;
    const u32 n   = g_online_cpu_count < kMaxCpus ? g_online_cpu_count : kMaxCpus;
    for (u32 i = 0; i < n; ++i) {
        if (!(allowed & (1ull << i))) continue;
        const u32 load = g_eki[i].latency_principals.size
                       + g_eki[i].vm_latency_principals.size
                       + g_eki[i].fair_principals.size
                       + g_eki[i].vm_fair_principals.size
                       + g_eki[i].batch_principals.size
                       + g_eki[i].idle_principals.size
                       + (g_eki[i].rt.bitmap ? 1u : 0u);
        if (load < best_load) { best_load = load; best_cpu = i; }
    }
    return best_cpu;
}

static u32 target_cpu_for(const Thread* t) {
    if (!t) return current_cpu_id();
    const u64 allowed = t->affinity_mask ? (t->affinity_mask & online_mask()) : online_mask();
    if (allowed & (1ull << t->cpu_id)) return t->cpu_id;
    return first_allowed_cpu(allowed);
}

u32 target_cpu_for_wake(Thread* waiter, Thread* waker,
                        u32 db_sched_hints, u32 db_home_cpu) {
    if (!waiter) return current_cpu_id();

    // Hard affinity: single allowed CPU
    const u64 allowed = waiter->affinity_mask
        ? (waiter->affinity_mask & online_mask()) : online_mask();
    if (__builtin_popcountll(allowed) == 1)
        return first_allowed_cpu(allowed);

    // Wake-affine: colocate with waker
    if ((db_sched_hints & IPC_HINT_WAKE_AFFINE) && waker) {
        const u32 wcpu = waker->cpu_id;
        if (wcpu < kMaxCpus && (allowed & (1ull << wcpu)))
            return wcpu;
    }

    // Home CPU hint from doorbell
    if (db_home_cpu < kMaxCpus && (allowed & (1ull << db_home_cpu)))
        return db_home_cpu;

    // Thread's home / last CPU
    if (waiter->home_cpu < kMaxCpus && (allowed & (1ull << waiter->home_cpu)))
        return waiter->home_cpu;
    if (waiter->last_cpu < kMaxCpus && (allowed & (1ull << waiter->last_cpu)))
        return waiter->last_cpu;

    return least_loaded_cpu_for(waiter);
}

// ---------------------------------------------------------------------------
// Apply IPC wake metadata — must be called before wake_local/wake_remote
// so that effective_key() in enqueue_fair reflects the credit.
// ---------------------------------------------------------------------------
void apply_wake_metadata(Thread* t, WakeReason reason, u32 sched_hints) {
    if (!t) return;
    t->last_wake_reason = reason;
    t->last_wake_ns     = sched_clock_ns();

    // Only doorbell and IRQ wakes get credit.  The credit remains non-transferable
    // per thread and is charged away as the receiver's principal runs.
    if (reason != WAKE_DOORBELL && reason != WAKE_IRQ) return;

    const u32 pid = t->principal_id;
    if (pid < kMaxPrincipalsLocal && g_principal_sched[pid].throttled) return;

    const u64 credit = wake_credit_for(t, reason, sched_hints);
    if (credit == 0) return;
    const i64 cap = static_cast<i64>(wake_credit_cap(t));
    t->wake_credit_ns = t->wake_credit_ns + static_cast<i64>(credit);
    if (t->wake_credit_ns > cap) t->wake_credit_ns = cap;

    // Mark IPC hot window for stickiness (resist work stealing)
    t->ipc_hot_until_ns = sched_clock_ns() + 2000000ULL; // 2 ms
}

// ---------------------------------------------------------------------------
// Wake preemption check — called after enqueueing a newly woken thread.
// Sets switch_pending if the woken thread should preempt the running one.
// ---------------------------------------------------------------------------
static void maybe_request_preemption_on_wake(Eki& e, Thread* woken) {
    Thread* cur = e.current;
    if (!cur || cur == e.idle) {
        request_switch(true, SWITCH_WAKE);
        return;
    }
    if (cur->state != TS_RUNNING) return;
    if (cur->sched.sched_class == SCHED_BAND_RT_NATIVE) return; // native RT never preempted by fair

    const u32 wbi = band_index(woken->sched.sched_class);
    const u32 cbi = band_index(cur->sched.sched_class);

    // Pick-order preemption: a higher band woke while a lower band runs.
    if (wbi < cbi) {
        request_switch(true, SWITCH_WAKE);
        metric_inc(e.ipc_wake_preemptions);
        return;
    }

    // Same-band margin-based preemption
    if (wbi == cbi) {
        const u64 margin  = wakeup_preempt_margin_ns(woken->sched.sched_class);
        if (margin > 0) {
            const u64 wk = effective_key(woken);
            const u64 ck = effective_key(cur);
            if (wk + margin < ck) {
                request_switch(true, SWITCH_WAKE);
                metric_inc(e.ipc_wake_preemptions);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Local wake — enqueue on current CPU's fair queue.
// apply_wake_metadata MUST be called before this.
// ---------------------------------------------------------------------------
void wake_local(Thread* t) {
    if (!t) return;
    Eki& e = current_eki();

    const ThreadState old = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (old == TS_DEAD || old == TS_RUNNING || old == TS_RUNNABLE_QUEUED
        || old == TS_RUNNABLE_PENDING_REMOTE) {
        return;
    }
    if (t->on_runqueue) return;

    t->state = TS_RUNNABLE_QUEUED;
    t->cpu_id = e.cpu_id;

    bool enqueued = false;
    if (t->sched.sched_class == SCHED_RT) {
        enqueue_rt(e, t);
        enqueued = t->on_runqueue;
    } else {
        enqueued = enqueue_fair(e, t);
    }

    metric_inc(e.local_wakes);
    if (t->last_wake_reason == WAKE_DOORBELL) metric_inc(e.wake_doorbell);
    else if (t->last_wake_reason == WAKE_RESUME) metric_inc(e.wake_resume);
    if (enqueued) maybe_request_preemption_on_wake(e, t);
    send_event();
}

// ---------------------------------------------------------------------------
// Remote wake — push to target CPU's pending queue and IPI.
// apply_wake_metadata MUST be called before this.
// ---------------------------------------------------------------------------
void wake_remote(Thread* t, u32 target_cpu) {
    if (!t) return;
    Eki& src = current_eki();
    if (target_cpu >= kMaxCpus) target_cpu = 0;

    const ThreadState old = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (old == TS_DEAD || old == TS_RUNNING || old == TS_RUNNABLE_QUEUED
        || old == TS_RUNNABLE_PENDING_REMOTE) {
        return;
    }
    if (t->on_runqueue) return;

    __atomic_store_n(&t->state,  TS_RUNNABLE_PENDING_REMOTE, __ATOMIC_RELEASE);
    __atomic_store_n(&t->cpu_id, target_cpu,                  __ATOMIC_RELEASE);
    pending_push(eki_for_cpu(target_cpu), t);
    metric_inc(src.remote_wakes);
    smp::send_wake_ipi(target_cpu);
}

// ---------------------------------------------------------------------------
// Backward-compatible wake (no IPC hints) — for resume, timeout, etc.
// ---------------------------------------------------------------------------
void wake(Thread* t) {
    if (!t) return;
    const ThreadState s = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    if (s == TS_DEAD || s == TS_RUNNING || s == TS_RUNNABLE_QUEUED
        || s == TS_RUNNABLE_PENDING_REMOTE) {
        return;
    }
    t->last_wake_reason = WAKE_NOTIFY;
    t->last_wake_ns     = sched_clock_ns();
    const u32 tgt = target_cpu_for(t);
    if (tgt == current_cpu_id()) wake_local(t);
    else                         wake_remote(t, tgt);
}

// ---------------------------------------------------------------------------
// Work stealing
// ---------------------------------------------------------------------------
static bool is_stealable(const Thread* t) {
    if (!t) return false;
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

static void handle_pending_steal_requests(Eki& victim) {
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
                __atomic_store_n(&g_eki[thief_cpu].steal_req_in_flight, false, __ATOMIC_RELEASE);
        }

        if (!t) {
            // Still need to free the request
            if (list->thief_cpu < kMaxCpus)
                __atomic_store_n(&g_eki[list->thief_cpu].steal_req_in_flight, false, __ATOMIC_RELEASE);
        }
        list = nxt;
    }
}

static void maybe_send_steal_request(Eki& thief) {
    if (g_online_cpu_count <= 1) return;

    // Don't send if already in flight
    if (__atomic_load_n(&thief.steal_req_in_flight, __ATOMIC_ACQUIRE)) return;

    // Find most loaded peer
    u32 victim_cpu = kInvalidCpu;
    u32 max_load   = 1; // only steal if victim has > 1 stealable thread
    const u32 n    = g_online_cpu_count < kMaxCpus ? g_online_cpu_count : kMaxCpus;
    for (u32 c = 0; c < n; ++c) {
        if (c == thief.cpu_id) continue;
        u32 load = g_eki[c].fair_principals.size + g_eki[c].vm_fair_principals.size + g_eki[c].batch_principals.size;
        if (load > max_load) { max_load = load; victim_cpu = c; }
    }
    if (victim_cpu == kInvalidCpu) return;

    __atomic_store_n(&thief.steal_req_in_flight, true, __ATOMIC_RELEASE);
    thief.steal_req_buf.thief_cpu = thief.cpu_id;
    thief.steal_req_buf.next      = nullptr;

    Eki& victim = g_eki[victim_cpu];
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

// ---------------------------------------------------------------------------
// Top-level pick
// ---------------------------------------------------------------------------
static Thread* pick_next(Eki& e) {
    drain_pending_wakes(e);
    handle_pending_steal_requests(e);

    if (Thread* t = pick_rt(e))                         return t;
    if (Thread* t = pick_fair(e, e.latency_principals))    return t;
    if (Thread* t = pick_fair(e, e.vm_latency_principals)) return t;
    if (Thread* t = pick_fair(e, e.fair_principals))       return t;
    if (Thread* t = pick_fair(e, e.vm_fair_principals))    return t;
    if (Thread* t = pick_fair(e, e.batch_principals))      return t;
    if (Thread* t = pick_fair(e, e.idle_principals))       return t;
    return nullptr;
}

// ---------------------------------------------------------------------------
// EL0 entry (initial first-run path)
// ---------------------------------------------------------------------------
[[noreturn]] static void enter_el0(VirtAddr entry, VirtAddr sp, u64 arg0) {
    asm volatile(
        "mov x0, %2\n"
        "msr sp_el0, %0\n"
        "msr elr_el1, %1\n"
        "mov x2, xzr\n"
        "msr spsr_el1, x2\n"
        "eret\n"
        :
        : "r"(sp), "r"(entry), "r"(arg0)
        : "x0", "x2", "memory");
    __builtin_unreachable();
}

static void activate_thread_space(Thread* t) {
    if (!t) return;
    if (Process* p = find_process(t->pid)) mm::vm_activate(p->ttbr0);
    else ANKO_LOGF_WARN("kernel/sched",
                        "activate: no process for tid=%u pid=%u", t->tid, t->pid);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void init_eki(u32 cpu_id) {
    Eki& e = g_eki[cpu_id];
    libk::memset(&e, 0, sizeof(e));
    e.cpu_id          = cpu_id;
    e.pending_reason  = SWITCH_NONE;
    e.latency_principals.band    = SCHED_BAND_LATENCY;
    e.vm_latency_principals.band = SCHED_BAND_VM_LATENCY;
    e.fair_principals.band       = SCHED_BAND_FAIR;
    e.vm_fair_principals.band    = SCHED_BAND_VM_FAIR;
    e.batch_principals.band      = SCHED_BAND_BATCH;
    e.idle_principals.band       = SCHED_BAND_IDLE;
    for (u32 i = 0; i < kMaxPrincipals; ++i) {
        e.principal_entities[i].principal_id = i;
        for (u32 b = 0; b < NUM_BANDS; ++b) e.principal_entities[i].heap_idx[b] = 0xff;
    }
}

void init() {
    ANKO_LOG_INFO("kernel/sched", "ressha/eki v2 init");
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(g_cntfrq));
    for (u32 i = 0; i < kMaxCpus; ++i) init_eki(i);

    // Reserve principal 0 for kernel/bootstrap work.  User processes and VMs get
    // unique IDs; PrincipalClass is now authorization metadata, not the fairness
    // bucket.
    g_principal_sched[0].principal_id = 0;
    g_principal_sched[0].class_ = PRINCIPAL_KERNEL;
    g_principal_sched[0].allocated = 1;
    g_principal_sched[0].weight = NICE_0_WEIGHT;
    g_principal_sched[0].period_start_ns = sched_clock_ns();

    // Mirror weights into per-CPU entities.
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        for (u32 i = 0; i < kMaxPrincipals; ++i)
            g_eki[cpu].principal_entities[i].weight = g_principal_sched[i].weight;
    }

    g_online_cpu_count = 1;
    mark_cpu_online(0);
    ANKO_LOG_INFO("kernel/sched", "eki[0] ready");
}

// ---------------------------------------------------------------------------
// Scheduler main loops
// ---------------------------------------------------------------------------
[[noreturn]] static void enter_cpu_scheduler(u32 cpu_id) {
    Eki& e = eki_for_cpu(cpu_id);
    for (;;) {
        drain_pending_wakes(e);
        handle_pending_steal_requests(e);
        Thread* next = pick_next(e);
        if (!next) {
            maybe_send_steal_request(e);
            wait_for_interrupt();
            continue;
        }

        e.current = next;
        next->state = TS_RUNNING;
        next->last_start_ns = sched_clock_ns();
        next->slice_start_ns = next->last_start_ns;
        activate_thread_space(next);
        ANKO_LOGF_INFO("kernel/sched",
                       "dispatch tid=%u pid=%u elr=%llx",
                       next->tid, next->pid,
                       static_cast<unsigned long long>(next->tf.elr));
        enter_el0(next->tf.elr, next->tf.sp_el0, next->tf.x[0]);
    }
}

[[noreturn]] void enter()               { enter_cpu_scheduler(current_cpu_id()); }
[[noreturn]] void enter_secondary(u32 c){ enter_cpu_scheduler(c); }

// ---------------------------------------------------------------------------
// Tick — accounting checkpoint; check slice expiry and budget.
// ---------------------------------------------------------------------------
void tick() {
    Eki& e  = current_eki();
    Thread* cur = e.current;
    if (!cur || cur->state != TS_RUNNING) return;
    metric_inc(e.timer_preempts);

    const u64 now = sched_clock_ns();
    const u64 ran_in_slice = (cur->slice_start_ns && now >= cur->slice_start_ns)
        ? (now - cur->slice_start_ns) : 0;

    account_current(e); // charges delta, updates vruntime, may set switch_pending
    refill_principal_budgets();
    refill_sched_context_budgets();

    if (e.switch_pending) return;
    if ((cur->slice_ns > 0 && ran_in_slice >= cur->slice_ns) ||
        (cur->handoff_deadline_ns != 0 && now >= cur->handoff_deadline_ns)) {
        request_switch(true, SWITCH_TIMER);
    }
}

// ---------------------------------------------------------------------------
// Yield / block
// ---------------------------------------------------------------------------
void yield() {
    metric_inc(g_yield_count);
    request_switch(true, SWITCH_YIELD);
}

void block(Thread* t) {
    if (!t) return;
    if (t->state != TS_SLEEPING) t->state = TS_BLOCKED;
    if (t == current_eki().current) request_switch(false, SWITCH_BLOCK);
}

// ---------------------------------------------------------------------------
// set_handoff_next — installs receiver as next thread for handoff
// ---------------------------------------------------------------------------
void set_handoff_next(Thread* receiver) {
    Eki& e         = current_eki();
    e.handoff_next = receiver;
}

bool principal_throttled(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return false;
    return __atomic_load_n(&g_principal_sched[pid].throttled, __ATOMIC_RELAXED);
}

bool switch_pending() {
    return current_eki().switch_pending;
}

u32 allocate_principal(PrincipalClass class_, u32 weight, u64 budget_ns, u64 period_ns) {
    if (g_next_principal_id >= kMaxPrincipalsLocal) return 0;
    const u32 id = g_next_principal_id++;
    PrincipalSchedState& ps = g_principal_sched[id];
    libk::memset(&ps, 0, sizeof(ps));
    ps.principal_id = id;
    ps.class_ = class_;
    ps.allocated = 1;
    ps.weight = weight ? weight : NICE_0_WEIGHT;
    ps.budget_ns = budget_ns;
    ps.period_ns = period_ns;
    ps.period_start_ns = sched_clock_ns();
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu)
        g_eki[cpu].principal_entities[id].weight = ps.weight;
    return id;
}

SchedContext* sched_context_by_id(u32 id) {
    if (id == 0) return nullptr;
    for (u32 i = 0; i < kMaxSchedContexts; ++i) {
        if (g_sched_context_allocated[i] && g_sched_contexts[i].id == id) return &g_sched_contexts[i];
    }
    return nullptr;
}

SchedContext* sched_context_from_cap(Process& caller, CapHandle sched_context_cap, SysError* out_error) {
    CapEntry* e = cap::lookup(*caller.cap_table, sched_context_cap, CAP_SCHED_CONTEXT);
    if (!e) { if (out_error) *out_error = E_NOTFOUND; return nullptr; }
    if ((e->rights_mask & RIGHT_SCHED_CONTEXT_CONTROL) == 0) {
        if (out_error) *out_error = E_PERM;
        return nullptr;
    }
    auto* ctx = static_cast<SchedContext*>(e->target);
    if (!ctx) { if (out_error) *out_error = E_NOTFOUND; return nullptr; }
    if (out_error) *out_error = E_OK;
    return ctx;
}

static bool valid_sched_band(SchedBand band) {
    return static_cast<u32>(band) < NUM_BANDS;
}

static bool sanitize_context_for_caller(Process& caller, SchedContext& ctx) {
    if (!valid_sched_band(ctx.band)) return false;
    if (ctx.weight == 0) ctx.weight = NICE_0_WEIGHT;
    if (ctx.core_mask == 0) ctx.core_mask = online_mask();
    ctx.core_mask &= online_mask();
    if (ctx.core_mask == 0) return false;
    if (ctx.home_cpu >= kMaxCpus || ((ctx.core_mask >> ctx.home_cpu) & 1u) == 0)
        ctx.home_cpu = first_allowed_cpu(ctx.core_mask);

    // Native hard RT is intentionally not available to sandboxed principals, and
    // VM contexts are never allowed to masquerade as hard RT.
    if (ctx.band == SCHED_BAND_RT_NATIVE && caller.principal_class != PRINCIPAL_KERNEL &&
        caller.principal_class != PRINCIPAL_NATIVE_TRUSTED) return false;

    if (ctx.principal_id == 0) ctx.principal_id = caller.principal_id;
    if (caller.principal_class != PRINCIPAL_KERNEL && ctx.principal_id != caller.principal_id)
        return false;
    if (ctx.principal_id >= kMaxPrincipalsLocal || !g_principal_sched[ctx.principal_id].allocated)
        return false;
    return true;
}

CapHandle sched_context_create(Process& caller, const SchedContext& request, SysError* out_error) {
    u32 slot = kMaxSchedContexts;
    for (u32 i = 0; i < kMaxSchedContexts; ++i) {
        if (!g_sched_context_allocated[i]) { slot = i; break; }
    }
    if (slot == kMaxSchedContexts) { if (out_error) *out_error = E_OVERFLOW; return kNullCap; }

    SchedContext ctx = request;
    if (!sanitize_context_for_caller(caller, ctx)) {
        if (out_error) *out_error = E_INVALID;
        return kNullCap;
    }

    ctx.id = g_next_sched_context_id++;
    ctx.used_ns = 0;
    ctx.period_start_ns = sched_clock_ns();
    g_sched_contexts[slot] = ctx;
    g_sched_context_allocated[slot] = true;

    CapHandle h = cap::alloc(*caller.cap_table, CAP_SCHED_CONTEXT, &g_sched_contexts[slot],
                             RIGHT_READ | RIGHT_DUP | RIGHT_SCHED_CONTEXT_CONTROL);
    if (h == kNullCap) {
        g_sched_context_allocated[slot] = false;
        if (out_error) *out_error = E_OVERFLOW;
        return kNullCap;
    }
    if (out_error) *out_error = E_OK;
    return h;
}

static bool bind_thread_to_context(Thread* t, SchedContext* ctx) {
    if (!t || !ctx) return false;
    if (t->state == TS_DEAD || t->on_runqueue || t->state == TS_RUNNING ||
        t->state == TS_RUNNABLE_PENDING_REMOTE || t->state == TS_RUNNABLE_QUEUED) return false;
    t->principal_id = ctx->principal_id;
    t->sched_context_id = ctx->id;
    t->sched_context = ctx;
    t->sched.sched_class = ctx->band;
    t->sched.weight = ctx->weight ? ctx->weight : NICE_0_WEIGHT;
    t->sched.slice_budget_ns = ctx->budget_ns;
    t->affinity_mask = ctx->core_mask;
    t->home_cpu = ctx->home_cpu;
    t->cpu_id = first_allowed_cpu(ctx->core_mask);
    if (ctx->band == SCHED_BAND_LATENCY) t->latency_hint = LATENCY_INTERACTIVE;
    else if (ctx->band == SCHED_BAND_BATCH) t->latency_hint = LATENCY_BATCH;
    else t->latency_hint = LATENCY_NORMAL;
    return true;
}

SysError sched_context_bind_thread(Process& caller, CapHandle thread_cap, CapHandle sched_context_cap) {
    CapEntry* te = cap::lookup(*caller.cap_table, thread_cap, CAP_THREAD);
    if (!te) return E_NOTFOUND;
    if ((te->rights_mask & RIGHT_THREAD_CONTROL) == 0) return E_PERM;
    auto* t = static_cast<Thread*>(te->target);
    SysError rc = E_OK;
    SchedContext* ctx = sched_context_from_cap(caller, sched_context_cap, &rc);
    if (!ctx) return rc;
    return bind_thread_to_context(t, ctx) ? E_OK : E_BUSY;
}

SysError bind_current_to_sched_context_id(u32 sched_context_id) {
    Thread* t = current();
    SchedContext* ctx = sched_context_by_id(sched_context_id);
    if (!t || !ctx) return E_NOTFOUND;
    // Current runner is already running, so bind only fields that affect future
    // accounting and re-enqueue.  Do not migrate it mid-syscall.
    t->principal_id = ctx->principal_id;
    t->sched_context_id = ctx->id;
    t->sched_context = ctx;
    t->sched.sched_class = ctx->band;
    t->sched.weight = ctx->weight ? ctx->weight : NICE_0_WEIGHT;
    t->sched.slice_budget_ns = ctx->budget_ns;
    t->affinity_mask = ctx->core_mask;
    t->home_cpu = ctx->home_cpu;
    return E_OK;
}

// ---------------------------------------------------------------------------
// Enqueue public API
// ---------------------------------------------------------------------------
void enqueue(Thread* t) {
    if (!t || t->state == TS_DEAD) return;
    if (t->state == TS_RUNNING || t->state == TS_RUNNABLE_PENDING_REMOTE) return;
    if (t->on_runqueue) return;
    t->state = TS_RUNNABLE_QUEUED;

    const u32 tgt = target_cpu_for(t);
    if (tgt == current_cpu_id()) {
        if (t->sched.sched_class == SCHED_BAND_RT_NATIVE) enqueue_rt(current_eki(), t);
        else                                             (void)enqueue_fair(current_eki(), t);
        send_event();
    } else {
        t->state = TS_RUNNABLE_PENDING_REMOTE;
        t->cpu_id = tgt;
        pending_push(eki_for_cpu(tgt), t);
        smp::send_wake_ipi(tgt);
    }
}

// ---------------------------------------------------------------------------
// Affinity
// ---------------------------------------------------------------------------
bool set_affinity(Thread* t, u64 cpu_mask) {
    if (!t || cpu_mask == 0) return false;
    if (t->state != TS_SUSPENDED) return false;
    const u64 allowed = cpu_mask & online_mask();
    if (!allowed) return false;
    t->affinity_mask = allowed;
    t->cpu_id = first_allowed_cpu(allowed);
    return true;
}

SysError resume_thread(Thread* t) {
    if (!t) return E_NOTFOUND;
    if (t->state == TS_DEAD) return E_DEAD;
    if (t->state != TS_SUSPENDED) return E_INVALID;
    t->last_wake_reason = WAKE_RESUME;
    enqueue(t);
    return E_OK;
}

// ---------------------------------------------------------------------------
// request_switch
// ---------------------------------------------------------------------------
void request_switch(bool requeue_cur, SwitchReason reason) {
    Eki& e = current_eki();
    e.switch_pending = true;
    if (reason == SWITCH_BLOCK || reason == SWITCH_EXIT
        || reason == SWITCH_BUDGET || reason == SWITCH_HANDOFF) {
        e.requeue_current = requeue_cur;
        e.pending_reason  = reason;
        return;
    }
    if (requeue_cur) e.requeue_current = true;
    if (e.pending_reason == SWITCH_NONE || e.pending_reason == SWITCH_IDLE)
        e.pending_reason = reason;
}

// ---------------------------------------------------------------------------
// mark_current_dead
// ---------------------------------------------------------------------------
void mark_current_dead() {
    Thread* cur = current();
    if (!cur) return;
    cur->state = TS_DEAD;
    request_switch(false, SWITCH_EXIT);
}



// ---------------------------------------------------------------------------
// post_syscall — called on every syscall return.
// Saves TrapFrame, performs context switch if needed.
// ---------------------------------------------------------------------------
extern "C" void post_syscall(void* frame_ptr) {
    irq_disable();
    Eki& e = current_eki();
    drain_pending_wakes(e);
    Thread* cur = e.current;
    if (!frame_ptr || !cur) {
        auto* tf = static_cast<TrapFrame*>(frame_ptr);
        ANKO_LOGF_WARN("kernel/sched",
                       "post_syscall: e.current=null frame=%p elr=%llx",
                       frame_ptr,
                       tf ? static_cast<unsigned long long>(tf->elr) : 0ull);
        return;
    }

    auto* frame = static_cast<TrapFrame*>(frame_ptr);
    cur->tf = *frame;

    if (!e.switch_pending && cur->state != TS_RUNNING) {
        e.switch_pending  = true;
        e.requeue_current = false;
        e.pending_reason  = (cur->state == TS_DEAD) ? SWITCH_EXIT : SWITCH_BLOCK;
    }

    if (!e.switch_pending) {
        activate_thread_space(cur);
        return;
    }
    e.switch_pending = false;
    const bool requeue   = e.requeue_current;
    const SwitchReason reason = e.pending_reason;
    e.requeue_current    = false;
    e.pending_reason     = SWITCH_NONE;

    // Charge current thread before potentially switching
    account_current(e);

    if (!requeue && cur->state == TS_RUNNING) {
        // No switch needed: thread continues, but the syscall path may have
        // temporarily activated another address space. Restore this thread's
        // user TTBR0 before returning to EL0.
        cur->last_start_ns = sched_clock_ns();
        activate_thread_space(cur);
        return;
    }

    if (requeue && cur->state == TS_RUNNING) {
        cur->state = TS_RUNNABLE_QUEUED;
        if (cur->sched.sched_class == SCHED_BAND_RT_NATIVE) enqueue_rt(e, cur);
        else enqueue_fair(e, cur);
    } else if (requeue && cur->state == TS_RUNNABLE_QUEUED) {
        if (cur->sched.sched_class == SCHED_BAND_RT_NATIVE) enqueue_rt(e, cur);
        else enqueue_fair(e, cur);
    }

    // Handoff override: skip fair pick
    Thread* next = e.handoff_next;
    e.handoff_next = nullptr;

    if (!next) {
        next = pick_next(e);
    }

    while (!next) {
        if (cur->state == TS_RUNNING || cur->state == TS_RUNNABLE_QUEUED) {
            next = cur;
            break;
        }
        e.current = nullptr;
        maybe_send_steal_request(e);
        wait_for_interrupt();
        drain_pending_wakes(e);
        next = pick_next(e);
    }

    if (next != cur) {
        metric_inc(e.context_switches);
        metric_inc(next->switches);
        switch (reason) {
        case SWITCH_YIELD:   metric_inc(e.yield_switches);  break;
        case SWITCH_BLOCK:   metric_inc(e.block_switches);  break;
        case SWITCH_EXIT:    metric_inc(e.exit_switches);   break;
        case SWITCH_IDLE:    metric_inc(e.idle_switches);   break;
        case SWITCH_HANDOFF: metric_inc(e.handoff_success); break;
        default: break;
        }
    }

    if (next->state != TS_RUNNABLE_QUEUED && next != cur) {
        // A stale queue entry escaped lazy filtering; keep current if possible.
        if (cur && cur->state == TS_RUNNING) next = cur;
    }

    e.current = next;
    next->state = TS_RUNNING;
    next->last_start_ns = sched_clock_ns();
    next->slice_start_ns = next->last_start_ns;
    next->last_cpu  = next->cpu_id;
    next->cpu_id    = e.cpu_id;

    // Set up IPC drain quantum on first run after doorbell wake
    if (next->last_wake_reason == WAKE_DOORBELL
        && next->latency_hint == LATENCY_IPC_SERVER
        && !next->in_drain_quantum) {
        next->in_drain_quantum       = true;
        next->drain_quantum_start_ns = next->last_start_ns;
    }

    activate_thread_space(next);
    *frame = next->tf;
    if (cur != next && cur->state == TS_DEAD) thread_destroy(cur);
}

// ---------------------------------------------------------------------------
// Thread query
// ---------------------------------------------------------------------------
Thread*  current() { return current_eki().current; }
Process* current_process() {
    Thread* t = current();
    return t ? find_process(t->pid) : nullptr;
}

// ---------------------------------------------------------------------------
// CPU management
// ---------------------------------------------------------------------------
u32 current_cpu_id() {
    // Do not use MPIDR_EL1 here.  Once EL2 virtualization is active,
    // VMPIDR_EL2 can affect what EL1 observes as MPIDR_EL1.  The host
    // scheduler needs a stable physical/logical CPU id, so we keep it in
    // TPIDR_EL1 on each host CPU.
    u64 cpu_id = 0;
    asm volatile("mrs %0, TPIDR_EL1" : "=r"(cpu_id));
    if (cpu_id >= kMaxCpus) return 0;
    return static_cast<u32>(cpu_id);
}

u32  online_cpu_count() { return g_online_cpu_count; }
void set_online_cpu_count(u32 n) {
    if (!n) n = 1;
    if (n > kMaxCpus) n = kMaxCpus;
    g_online_cpu_count = n;
}
void mark_cpu_online(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) return;
    if (cpu_id + 1 > g_online_cpu_count) g_online_cpu_count = cpu_id + 1;
    // Mirror principal weights to secondary CPU Eki
    for (u32 i = 0; i < kMaxPrincipals; ++i)
        g_eki[cpu_id].principal_entities[i].weight = g_principal_sched[i].weight;
}
void note_ipi_sent(u32)   { metric_inc(current_eki().ipis_sent); }
void note_ipi_handled()   { metric_inc(current_eki().ipis_handled); }
void drain_pending_wakes() { drain_pending_wakes(current_eki()); }

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------
u64 context_switch_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].context_switches);
    return t;
}
u64 cpu_context_switch_count(u32 c) {
    return c < kMaxCpus ? metric_load(g_eki[c].context_switches) : 0;
}
u64 yield_count()        { return metric_load(g_yield_count); }
u64 block_switch_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].block_switches);
    return t;
}
u64 timer_preempt_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].timer_preempts);
    return t;
}
u64 cpu_timer_preempt_count(u32 c) {
    return c < kMaxCpus ? metric_load(g_eki[c].timer_preempts) : 0;
}
u64 exit_switch_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].exit_switches);
    return t;
}
u64 local_wake_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].local_wakes);
    return t;
}
u64 remote_wake_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].remote_wakes);
    return t;
}
u64 ipi_sent_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].ipis_sent);
    return t;
}
u64 ipi_handled_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].ipis_handled);
    return t;
}
u64 pending_wake_drain_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].pending_wake_drains);
    return t;
}
u64 principal_runtime(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return 0;
    return __atomic_load_n(&g_principal_sched[pid].runtime_ns, __ATOMIC_RELAXED);
}

u64 principal_class_runtime(PrincipalClass class_) {
    u64 total = 0;
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        PrincipalSchedState& ps = g_principal_sched[i];
        if (ps.allocated && ps.class_ == class_) {
            total += __atomic_load_n(&ps.runtime_ns, __ATOMIC_RELAXED);
        }
    }
    return total;
}
u64 fairness_max_lag() {
    u64 min_v = ~0ull, max_v = 0;
    bool any = false;
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        const u64 rt = __atomic_load_n(&g_principal_sched[i].runtime_ns, __ATOMIC_RELAXED);
        if (!rt) continue;
        any = true;
        if (rt < min_v) min_v = rt;
        if (rt > max_v) max_v = rt;
    }
    return any ? (max_v - min_v) : 0;
}
u64 wake_doorbell_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].wake_doorbell);
    return t;
}
u64 wake_remote_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].wake_remote);
    return t;
}
u64 wake_resume_count() {
    u64 t = 0;
    for (u32 i = 0; i < kMaxCpus; ++i) t += metric_load(g_eki[i].wake_resume);
    return t;
}

} // namespace sched

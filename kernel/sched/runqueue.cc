// kernel/sched/runqueue.cc — Ressha/Eki V2 per-CPU fair scheduler.
//
// Pick order: RT (bitmap) → latency-fair (INTERACTIVE+SERVICE) →
//             timeshare-fair (NORMAL+BATCH+VM) → steal request → idle.
// Fairness is hierarchical: pick principal (min vruntime), then pick thread
// (min effective_key = vruntime - min(wake_credit, class_cap)).

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

static void pending_push(Eki& target, Thread* t);

using libk::metric_inc;
using libk::metric_load;

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
static inline u64 sched_clock_ns() {
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
// Class helpers
// ---------------------------------------------------------------------------
static bool is_latency_class(const Thread* t) {
    return t->sched.sched_class == SCHED_INTERACTIVE
        || t->sched.sched_class == SCHED_SERVICE;
}

static u64 wake_credit_cap(SchedClass cls) {
    switch (cls) {
    case SCHED_INTERACTIVE: return 300000ULL; // 300 us
    case SCHED_SERVICE:     return 200000ULL; // 200 us
    case SCHED_NORMAL:      return  75000ULL; //  75 us (only with IPC_HINT_LATENCY)
    default:                return 0ULL;
    }
}

static u64 wake_credit_for(SchedClass cls, u32 hints) {
    if (cls == SCHED_BATCH || cls == SCHED_VM || cls == SCHED_IDLE || cls == SCHED_RT)
        return 0;
    if (cls == SCHED_NORMAL && !(hints & IPC_HINT_LATENCY))
        return 0;
    return wake_credit_cap(cls);
}

// Default per-class target latency / min granularity / max slice (all ns)
static u64 class_target_latency_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_INTERACTIVE: return  4000000ULL; //  4 ms
    case SCHED_SERVICE:     return  4000000ULL; //  4 ms
    case SCHED_NORMAL:      return 12000000ULL; // 12 ms
    case SCHED_BATCH:       return 24000000ULL; // 24 ms
    case SCHED_VM:          return 12000000ULL; // 12 ms (same as NORMAL)
    default:                return 12000000ULL;
    }
}
static u64 class_min_gran_ns(SchedClass cls) {
    (void)cls;
    return 500000ULL; // 0.5 ms for all classes
}
static u64 class_max_slice_ns(SchedClass cls) {
    (void)cls;
    return 8000000ULL; // 8 ms hard cap
}

static u64 wakeup_preempt_margin_ns(SchedClass cls) {
    switch (cls) {
    case SCHED_INTERACTIVE:
    case SCHED_SERVICE:     return  50000ULL; //  50 us
    case SCHED_NORMAL:      return 250000ULL; // 250 us
    default:                return 0ULL;      // no wake preemption for batch/VM
    }
}

constexpr u64 kIpcDrainQuantumNs = 100000ULL; // 100 us

// ---------------------------------------------------------------------------
// Effective pick key for fair queue ordering
// ---------------------------------------------------------------------------
static u64 effective_key(const Thread* t) {
    const i64 cap    = static_cast<i64>(wake_credit_cap(t->sched.sched_class));
    const i64 credit = t->wake_credit_ns < cap ? t->wake_credit_ns : cap;
    const i64 key    = static_cast<i64>(t->vruntime_ns) - credit;
    u64 k = static_cast<u64>(key < 0 ? 0 : key);
    // Non-transitive priority donation from handoff sender
    if (t->has_donation && t->donated_priority < k) k = t->donated_priority;
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
// FairPrincipalQueue min-heap (keyed by vruntime_ns)
// ---------------------------------------------------------------------------
static void fpq_sift_up(FairPrincipalQueue& q, u32 i) {
    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (q.data[parent]->vruntime_ns <= q.data[i]->vruntime_ns) break;
        PrincipalEntity* tmp = q.data[parent];
        q.data[parent] = q.data[i];
        q.data[i]      = tmp;
        q.data[parent]->heap_idx = static_cast<u8>(parent);
        q.data[i]->heap_idx      = static_cast<u8>(i);
        i = parent;
    }
}

static void fpq_sift_down(FairPrincipalQueue& q, u32 i) {
    while (true) {
        u32 left     = 2 * i + 1;
        u32 right    = 2 * i + 2;
        u32 smallest = i;
        if (left  < q.size && q.data[left]->vruntime_ns  < q.data[smallest]->vruntime_ns) smallest = left;
        if (right < q.size && q.data[right]->vruntime_ns < q.data[smallest]->vruntime_ns) smallest = right;
        if (smallest == i) break;
        PrincipalEntity* tmp = q.data[i];
        q.data[i]            = q.data[smallest];
        q.data[smallest]     = tmp;
        q.data[i]->heap_idx      = static_cast<u8>(i);
        q.data[smallest]->heap_idx = static_cast<u8>(smallest);
        i = smallest;
    }
}

static void fpq_push(FairPrincipalQueue& q, PrincipalEntity* pe) {
    if (q.size >= kMaxPrincipals) return;
    q.data[q.size] = pe;
    pe->heap_idx   = static_cast<u8>(q.size);
    ++q.size;
    fpq_sift_up(q, q.size - 1);
}

static void fpq_rebuild(FairPrincipalQueue& q) {
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
        if (q.data[i]) q.data[i]->heap_idx = static_cast<u8>(i);
    }
}

static PrincipalEntity* fpq_pop_min(FairPrincipalQueue& q) {
    if (q.size == 0) return nullptr;

    // Corruption guard.  The observed crash was q.size underflowing and the
    // generated load indexing q.data[0xffffffff].
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
            moved->heap_idx = 0;
            fpq_sift_down(q, 0);
        } else {
            fpq_rebuild(q);
        }
    } else {
        q.data[0] = nullptr;
    }

    if (min) min->heap_idx = 0xff;
    return min;
}

static PrincipalEntity* fpq_peek_min(const FairPrincipalQueue& q) {
    return q.size ? q.data[0] : nullptr;
}

// ---------------------------------------------------------------------------
// Band routing: which FairPrincipalQueue does this thread belong to?
// ---------------------------------------------------------------------------
static FairPrincipalQueue& band_for(Eki& e, SchedClass cls) {
    return (cls == SCHED_INTERACTIVE || cls == SCHED_SERVICE)
        ? e.latency_principals : e.timeshare_principals;
}
static u64& min_vruntime_for(Eki& e, SchedClass cls) {
    return (cls == SCHED_INTERACTIVE || cls == SCHED_SERVICE)
        ? e.min_vruntime_latency : e.min_vruntime_timeshare;
}

// ---------------------------------------------------------------------------
// enqueue_fair — add thread to its principal entity's fair queue.
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

    PrincipalEntity& pe      = e.principal_entities[pid];
    FairPrincipalQueue& band = band_for(e, t->sched.sched_class);
    u64& min_vr             = min_vruntime_for(e, t->sched.sched_class);

    // Normalize: clamp back-pay from sleeping.
    if (t->vruntime_ns < min_vr) t->vruntime_ns = min_vr;
    if (pe.vruntime_ns < min_vr) pe.vruntime_ns = min_vr;

    if (pe.weight == 0) pe.weight = t->sched.weight ? t->sched.weight : NICE_0_WEIGHT;
    t->slice_ns = compute_slice_ns(t, pe.runnable_threads + 1);

    if (!ftq_push(pe.threads, t)) {
        // Do not feed full queues back into pending_wakes: that creates a
        // self-sustaining duplicate runnable entry.  Leave the thread runnable
        // but off-queue so the fault is visible through lag/metrics instead of
        // corrupting another thread's trapframe.
        t->on_runqueue = false;
        return false;
    }

    t->on_runqueue = true;
    t->cpu_id = e.cpu_id;
    ++pe.runnable_threads;

    bool& in_q = (&band == &e.latency_principals)
        ? pe.in_latency_queue : pe.in_timeshare_queue;
    if (!in_q) {
        fpq_push(band, &pe);
        in_q = true;
    }

    if (Process* p = find_process(t->pid)) ++p->runnable_threads;
    return true;
}

// ---------------------------------------------------------------------------
// dequeue_from_principal — remove thread from its principal's heap.
// Called by work stealing.
// ---------------------------------------------------------------------------
static bool dequeue_from_principal(Eki& e, Thread* t) {
    if (!t) return false;
    const u32 pid = t->principal_id;
    if (pid >= kMaxPrincipals) return false;

    PrincipalEntity& pe = e.principal_entities[pid];
    if (!ftq_remove(pe.threads, t)) return false;
    if (pe.runnable_threads > 0) --pe.runnable_threads;

    // If principal is now empty and was in a band, remove it.
    // (band removal done lazily in pick_fair)

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
// pick_fair — pick min-vruntime principal, then min-effective-key thread.
// Skips throttled principals (lazily removes them from band).
// ---------------------------------------------------------------------------
static Thread* pick_fair(Eki& e, FairPrincipalQueue& band) {
    while (band.size > 0) {
        PrincipalEntity* pe = fpq_peek_min(band);
        if (!pe) break;

        if (pe->throttled) {
            fpq_pop_min(band);
            if (&band == &e.latency_principals) pe->in_latency_queue = false;
            else                                pe->in_timeshare_queue = false;
            continue;
        }

        Thread* t = ftq_pop_min(pe->threads);
        if (!t) {
            fpq_pop_min(band);
            if (&band == &e.latency_principals) pe->in_latency_queue = false;
            else                                pe->in_timeshare_queue = false;
            continue;
        }

        if (pe->runnable_threads > 0) --pe->runnable_threads;
        if (Process* p = find_process(t->pid)) {
            if (p->runnable_threads > 0) --p->runnable_threads;
        }

        fpq_pop_min(band);
        if (pe->runnable_threads > 0) {
            fpq_push(band, pe);
        } else {
            if (&band == &e.latency_principals) pe->in_latency_queue = false;
            else                                pe->in_timeshare_queue = false;
        }

        u64& min_vr = (&band == &e.latency_principals)
            ? e.min_vruntime_latency : e.min_vruntime_timeshare;
        if (pe->vruntime_ns > min_vr) min_vr = pe->vruntime_ns;

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

        // Per-CPU principal entity vruntime
        PrincipalEntity& pe = e.principal_entities[pid];
        pe.runtime_ns   += delta;
        const u32 pw     = pe.weight ? pe.weight : NICE_0_WEIGHT;
        pe.vruntime_ns  += delta * NICE_0_WEIGHT / pw;

        // Thread vruntime
        const u32 tw     = t->sched.weight ? t->sched.weight : NICE_0_WEIGHT;
        t->vruntime_ns  += delta * NICE_0_WEIGHT / tw;

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
        if (now - ps.period_start_ns < ps.period_ns) continue;
        ps.used_in_period_ns = 0;
        ps.period_start_ns   = now;
        if (ps.throttled) {
            ps.throttled = false;
            // Unthrottle on current CPU; remote CPUs will see it on next pick
            Eki& e = current_eki();
            if (i < kMaxPrincipals) e.principal_entities[i].throttled = false;
        }
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
                       + g_eki[i].timeshare_principals.size
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

    // Only doorbell and IRQ wakes get credit
    if (reason != WAKE_DOORBELL && reason != WAKE_IRQ) return;
    if (t->sched.sched_class == SCHED_BATCH
        || t->sched.sched_class == SCHED_VM
        || t->sched.sched_class == SCHED_IDLE
        || t->sched.sched_class == SCHED_RT) return;

    const bool hint_latency = (sched_hints & IPC_HINT_LATENCY) != 0;
    const bool server_hint  = (t->latency_hint == LATENCY_IPC_SERVER);
    if (!hint_latency && !server_hint) return;

    const u32 pid = t->principal_id;
    if (pid < kMaxPrincipalsLocal && g_principal_sched[pid].throttled) return;

    const u64 credit = wake_credit_for(t->sched.sched_class, sched_hints);
    if (credit == 0) return;
    const i64 cap = static_cast<i64>(wake_credit_cap(t->sched.sched_class));
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
    if (cur->sched.sched_class == SCHED_RT) return; // RT never preempted by fair

    // Band preemption: latency thread woke while timeshare runs
    if (is_latency_class(woken) && !is_latency_class(cur)) {
        request_switch(true, SWITCH_WAKE);
        metric_inc(e.ipc_wake_preemptions);
        return;
    }

    // Same-band margin-based preemption
    if (is_latency_class(woken) == is_latency_class(cur)) {
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
    if (t->sched.sched_class == SCHED_RT)  return false;
    // Hard affinity: pinned to one CPU
    if (t->affinity_mask != 0 && __builtin_popcountll(t->affinity_mask) == 1) return false;
    // Service threads with no-steal flag
    if (t->sched.sched_class == SCHED_SERVICE &&
        (t->ipc_hot_until_ns > sched_clock_ns())) return false;
    // VM threads with single-CPU affinity — don't steal pinned vCPUs
    if (t->sched.sched_class == SCHED_VM &&
        t->affinity_mask != 0 && __builtin_popcountll(t->affinity_mask) == 1) return false;
    return t->sched.sched_class == SCHED_NORMAL
        || t->sched.sched_class == SCHED_BATCH
        || t->sched.sched_class == SCHED_VM;
}

static Thread* select_stealable(Eki& victim) {
    // Prefer timeshare band for stealing
    for (u32 i = 0; i < victim.timeshare_principals.size; ++i) {
        PrincipalEntity* pe = victim.timeshare_principals.data[i];
        for (u32 j = 0; j < pe->threads.size; ++j) {
            if (is_stealable(pe->threads.data[j])) return pe->threads.data[j];
        }
    }
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
        u32 load = g_eki[c].timeshare_principals.size;
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

    if (Thread* t = pick_rt(e))                     return t;
    if (Thread* t = pick_fair(e, e.latency_principals))  return t;
    if (Thread* t = pick_fair(e, e.timeshare_principals)) return t;
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
    for (u32 i = 0; i < kMaxPrincipals; ++i)
        e.principal_entities[i].principal_id = i;
}

void init() {
    ANKO_LOG_INFO("kernel/sched", "ressha/eki v2 init");
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(g_cntfrq));
    for (u32 i = 0; i < kMaxCpus; ++i) init_eki(i);

    // Initialize global principal weights
    g_principal_sched[PRINCIPAL_KERNEL].principal_id           = PRINCIPAL_KERNEL;
    g_principal_sched[PRINCIPAL_KERNEL].weight                 = NICE_0_WEIGHT;
    g_principal_sched[PRINCIPAL_NATIVE_TRUSTED].principal_id   = PRINCIPAL_NATIVE_TRUSTED;
    g_principal_sched[PRINCIPAL_NATIVE_TRUSTED].weight         = 2048; // service-class
    g_principal_sched[PRINCIPAL_NATIVE_SANDBOXED].principal_id = PRINCIPAL_NATIVE_SANDBOXED;
    g_principal_sched[PRINCIPAL_NATIVE_SANDBOXED].weight       = NICE_0_WEIGHT;

    // Mirror weights into per-CPU entities on CPU 0
    for (u32 i = 0; i < kMaxPrincipals; ++i)
        g_eki[0].principal_entities[i].weight = g_principal_sched[i].weight;

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

    account_current(e); // charges delta, updates vruntime, may set switch_pending
    refill_principal_budgets();

    // Always request requeue on timer tick — fair pick will choose who runs next.
    request_switch(true, SWITCH_TIMER);
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
        if (t->sched.sched_class == SCHED_RT) enqueue_rt(current_eki(), t);
        else                                  (void)enqueue_fair(current_eki(), t);
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
        enqueue_fair(e, cur);
    } else if (requeue && cur->state == TS_RUNNABLE_QUEUED) {
        enqueue_fair(e, cur);
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

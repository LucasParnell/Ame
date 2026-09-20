// kernel/sched/runqueue.cc — Ressha/Eki V2 per-CPU fair scheduler.
//
// Pick order: native RT → native latency → VM latency → native fair →
//             VM fair → batch → idle.
// Fairness is hierarchical: pick principal (per-band min vruntime), then pick
// a thread/vCPU runner from that same band-specific principal queue.

#include "anko/sched.h"
#include "sched_internal.h"
#include "anko/ipc.h"
#include "anko/mm.h"
#include "anko/smp.h"
#include "anko/log.h"
#include "anko/panic.h"
#include "anko/spinlock.h"
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
static u64   g_wrong_cpu_dequeues = 0;

constexpr u32 kMaxPrincipalsLocal = kMaxPrincipals; // alias for clarity
static PrincipalSchedState g_principal_sched[kMaxPrincipalsLocal]{};
static u32 g_next_principal_id = 1; // principal 0 is reserved for the kernel/bootstrap bucket
static kernel::SpinLock g_principal_lock{};

bool principal_allocated(u32 pid) {
    return pid < kMaxPrincipalsLocal && g_principal_sched[pid].allocated;
}


static void pending_push(Eki& target, Thread* t);

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
    asm volatile("wfe" ::: "memory");
    irq_disable();
}

static Eki& eki_for_cpu(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
    return g_eki[cpu_id];
}
static Eki& current_eki() { return eki_for_cpu(current_cpu_id()); }

u64 online_mask() {
    const u32 n = g_online_cpu_count > kMaxCpus ? kMaxCpus : g_online_cpu_count;
    u64 m = 0;
    for (u32 i = 0; i < n; ++i) m |= (1ull << i);
    return m ? m : 1ull;
}

// Scheduling policy (sched_class.cc) and fair-queue data structures
// (fair_queue.cc) were split out of this file; see sched_internal.h.
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

    if (auto p = find_process(t->pid)) ++p->runnable_threads;
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

    if (auto p = find_process(t->pid)) {
        if (p->runnable_threads > 0) --p->runnable_threads;
    }
    return true;
}

// Public: remove a thread from its CPU's fair queue (safe to call before TS_DEAD).
void dequeue_thread(Thread* t) {
    if (!t) return;
    if (!t->on_runqueue && t->state != TS_RUNNABLE_PENDING_REMOTE) return;
    const u32 cpu = (t->cpu_id < kMaxCpus) ? t->cpu_id : 0;
    ANKO_ASSERT(cpu == current_cpu_id());
    if (t->sched.sched_class == SCHED_BAND_RT_NATIVE) {
        Eki& e = g_eki[cpu];
        for (u32 i = 0; i < kMaxRtPrio; ++i) {
            auto& q = e.rt.queues[i]; Thread* prev = nullptr;
            for (Thread* at = q.head; at; at = at->next) {
                if (at == t) {
                    if (prev) prev->next = at->next; else q.head = at->next;
                    if (q.tail == at) q.tail = prev;
                    if (q.count) --q.count;
                    if (!q.head) e.rt.bitmap &= ~(1u << i);
                    t->next = nullptr; t->on_runqueue = false; return;
                }
                prev = at;
            }
        }
    }
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
                            && (t->cpu_id == e.cpu_id);
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
        if (auto p = find_process(t->pid)) {
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
    if (auto p = find_process(t->pid)) p->runtime_ns += delta;

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
        const u32 bi = band_index(t->sched.sched_class);
        pe.vruntime[bi] += delta * NICE_0_WEIGHT / pw;
        if (pe.in_queue[bi]) fpq_resift(band_for(e, t->sched.sched_class), &pe);

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


static void maybe_request_preemption_on_wake(Eki& e, Thread* woken);

// ---------------------------------------------------------------------------
// Pending-wake drain — MPSC stack → FIFO reverse → enqueue into local runqueue.
// Remote wake IPIs must request a switch after queueing a runnable thread;
// otherwise the interrupted EL0 thread simply resumes and a CPU-bound service
// can starve a newly-resumed VM runner indefinitely.
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

        bool enqueued = false;
        if (t->sched.sched_class == SCHED_BAND_RT_NATIVE) {
            enqueue_rt(e, t);
            enqueued = t->on_runqueue;
        } else {
            enqueued = enqueue_fair(e, t);
        }
        if (enqueued) {
            maybe_request_preemption_on_wake(e, t);
        }
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
u32 first_allowed_cpu(u64 allowed) {
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
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    if (__atomic_load_n(&t->terminate_requested, __ATOMIC_ACQUIRE)) return;
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
    if (t->sched.sched_class == SCHED_BAND_RT_NATIVE) {
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
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    if (__atomic_load_n(&t->terminate_requested, __ATOMIC_ACQUIRE)) return;
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

void collect_terminations(Eki& e, Thread* protected_thread) {
    if (!termination_pending()) return;
    for (u32 tid = 1; tid <= 128; ++tid) {
        auto t = find_thread(tid);
        if (!t || t == protected_thread || t == e.current ||
            !__atomic_load_n(&t->terminate_requested, __ATOMIC_ACQUIRE)) continue;
        {
            kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
            if (t->cpu_id != e.cpu_id || t->state == TS_RUNNING || t->state == TS_RUNNABLE_PENDING_REMOTE) continue;
            dequeue_thread(t);
            if (e.handoff_next == t) e.handoff_next = nullptr;
            t->state = TS_DEAD;
        }
        if (t->reply_target) {
            Thread* peer = t->reply_target;
            t->reply_target = nullptr;
            if (peer->reply_target == t) {
                peer->reply_target = nullptr;
                peer->tf.x[0] = static_cast<u64>(E_DEAD);
                wake(peer);
            }
        }
        finish_thread(t);
    }
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
    if (auto p = find_process(t->pid)) mm::vm_activate(p->ttbr0);
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
        collect_terminations(e);
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
    kernel::LockGuard guard(g_principal_lock);
    u32 id = 0;
    for (u32 scan = 0; scan < kMaxPrincipalsLocal - 1; ++scan) {
        const u32 candidate = ((g_next_principal_id - 1 + scan) % (kMaxPrincipalsLocal - 1)) + 1;
        if (!g_principal_sched[candidate].allocated) {
            id = candidate;
            g_next_principal_id = (candidate % (kMaxPrincipalsLocal - 1)) + 1;
            break;
        }
    }
    if (id == 0) return 0;
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

void release_principal(u32 id) {
    if (id == 0 || id >= kMaxPrincipalsLocal) return;
    kernel::LockGuard guard(g_principal_lock);
    libk::memset(&g_principal_sched[id], 0, sizeof(g_principal_sched[id]));
    if (id < g_next_principal_id) g_next_principal_id = id;
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        PrincipalEntity& pe = g_eki[cpu].principal_entities[id];
        libk::memset(&pe, 0, sizeof(pe));
        pe.principal_id = id;
        for (u32 b = 0; b < NUM_BANDS; ++b) pe.heap_idx[b] = 0xff;
    }
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
void mark_current_dead(bool whole_process) {
    Thread* cur = current();
    if (!cur) return;
    if (whole_process) process_destroy(cur->owner);
    else thread_destroy(cur);
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
    collect_terminations(e, cur);
    if (cur && __atomic_load_n(&cur->terminate_requested, __ATOMIC_ACQUIRE)) cur->state = TS_DEAD;
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
        if (cur && (cur->state == TS_RUNNING || cur->state == TS_RUNNABLE_QUEUED)) {
            next = cur;
            break;
        }
        e.current = nullptr;
        if (cur && cur->state == TS_DEAD) {
            // This CPU may have no successor. Leave the dying address space
            // before acknowledging teardown, then stop retaining its Thread.
            mm::vm_activate(0);
            finish_thread(cur);
            cur = nullptr;
        }
        maybe_send_steal_request(e);
        wait_for_interrupt();
        drain_pending_wakes(e);
        collect_terminations(e, cur);
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
    if (cur && cur != next && cur->state == TS_DEAD) finish_thread(cur);
}

// ---------------------------------------------------------------------------
// Thread query
// ---------------------------------------------------------------------------
Thread*  current() { return current_eki().current; }
Process* current_process() {
    Thread* t = current();
    return t ? t->owner : nullptr;
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
u64 wrong_cpu_dequeue_count() {
    return metric_load(g_wrong_cpu_dequeues);
}

} // namespace sched

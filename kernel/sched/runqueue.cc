// kernel/sched/runqueue.cc — Ressha/Eki V2 per-CPU fair scheduler.
//
// Native RT retains fixed priority. Other bands share a principal service
// clock, then a weighted thread clock; band precedence breaks equal-key ties.
// Fairness is hierarchical: pick principal (per-band min vruntime), then pick
// a thread/vCPU runner from that same band-specific principal queue.

#include "anko/sched.h"
#include "anko/platform_clock.h"
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
static u64   g_online_cpu_mask = 1;
static u64   g_cntfrq = 0;          // cached at init

using libk::metric_inc;

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

// Only enable IRQs for the event wait. Scheduler queue mutation must
// resume with IRQs masked, otherwise timer/IPI paths can reenter queue code.
static inline void wait_for_interrupt()  {
    irq_enable();
    asm volatile("wfe" ::: "memory");
    irq_disable();
}

Eki& eki_for_cpu(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) cpu_id = 0;
    return g_eki[cpu_id];
}
Eki& current_eki() { return eki_for_cpu(current_cpu_id()); }
static u64 active_ttbr0();

u64 online_mask() {
    return __atomic_load_n(&g_online_cpu_mask, __ATOMIC_ACQUIRE);
}

// Scheduling policy (sched_class.cc) and fair-queue data structures
// (fair_queue.cc) were split out of this file; see sched_internal.h.
// ---------------------------------------------------------------------------
// enqueue_fair — add thread to its principal entity's band-specific fair queue.
// Normalize principal and thread time against their separate local clocks so
// sleepers receive bounded catch-up without erasing relative thread weights.
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
    const u32 generation = principal_generation(pid);
    if (!generation || (t->principal_generation && t->principal_generation != generation)) return false;
    t->principal_generation = generation;
    if (pe.generation != generation) {
        for (u32 b = 1; b < NUM_BANDS; ++b) {
            ANKO_ASSERT(band_threads(pe, static_cast<SchedBand>(b)).size == 0);
            if (pe.in_queue[b]) fpq_remove(band_for(e, static_cast<SchedBand>(b)), &pe);
        }
        pe = {};
        pe.principal_id = pid;
        pe.generation = generation;
        for (u32 b = 0; b < NUM_BANDS; ++b) pe.heap_idx[b] = 0xff;
    }
    if (t->migration_rebase) {
        const u64 floor = pe.thread_min_vruntime_ns;
        t->vruntime_ns = t->vruntime_ns > ~0ull - floor ? ~0ull : t->vruntime_ns + floor;
        t->migration_rebase = false;
    }
    if (t->vruntime_ns < pe.thread_min_vruntime_ns)
        t->vruntime_ns = pe.thread_min_vruntime_ns;
    if (pe.service_vruntime_ns < e.min_service_vruntime_ns)
        pe.service_vruntime_ns = e.min_service_vruntime_ns;
    if (pe.vruntime[bi] < min_vr) pe.vruntime[bi] = min_vr;

    const u32 global_weight = principal_weight(pid);
    pe.weight = global_weight;
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
    ++e.queued_threads[bi];
    publish_queue_load(e);

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
            --e.queued_threads[bi];
            publish_queue_load(e);
            t->on_runqueue = false;
            t->state = TS_BLOCKED;
            return false;
        }
    }

    if (t->owner) __atomic_add_fetch(&t->owner->runnable_threads, 1u, __ATOMIC_RELAXED);
    return true;
}

// ---------------------------------------------------------------------------
// dequeue_from_principal — remove thread from its principal's band heap.
// Called by work stealing and destruction.
// ---------------------------------------------------------------------------
bool dequeue_from_principal(Eki& e, Thread* t) {
    if (!t) return false;
    const u32 pid = t->principal_id;
    if (pid >= kMaxPrincipals) return false;

    const SchedBand sb = t->sched.sched_class;
    const u32 bi = band_index(sb);
    PrincipalEntity& pe = e.principal_entities[pid];
    BandQueue& threads = band_threads(pe, sb);
    if (!ftq_remove(threads, t)) return false;
    if (pe.runnable[bi] > 0) --pe.runnable[bi];

    if (e.queued_threads[bi]) --e.queued_threads[bi];
    publish_queue_load(e);
    if (!threads.size && pe.in_queue[bi]) fpq_remove(band_for(e, sb), &pe);
    if (t->owner) __atomic_sub_fetch(&t->owner->runnable_threads, 1u, __ATOMIC_RELAXED);
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
                    if (e.queued_threads[0]) --e.queued_threads[0];
                    publish_queue_load(e);
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
    ++e.queued_threads[0];
    publish_queue_load(e);
    e.rt.bitmap |= (1u << prio);
    t->on_runqueue = true;
    t->cpu_id = e.cpu_id;
}

void publish_queue_load(Eki& e) {
    u32 total = 0;
    for (u32 b = 0; b < NUM_BANDS; ++b) total += e.queued_threads[b];
    __atomic_store_n(&e.published_load, total, __ATOMIC_RELEASE);
    const u32 lower = e.queued_threads[SCHED_BAND_FAIR] +
        e.queued_threads[SCHED_BAND_VM_FAIR] + e.queued_threads[SCHED_BAND_BATCH];
    __atomic_store_n(&e.published_steal_load, lower, __ATOMIC_RELEASE);
}

static Thread* pick_rt(Eki& e) {
    for (u32 priority = kMaxRtPrio; priority > 0; --priority) {
        auto& q = e.rt.queues[priority - 1];
        const u32 attempts = q.count;
        for (u32 i = 0; i < attempts && q.head; ++i) {
            Thread* t = q.head;
            q.head = t->next;
            if (!q.head) { q.tail = nullptr; e.rt.bitmap &= ~(1u << (priority - 1)); }
            --q.count;
            if (e.queued_threads[0]) --e.queued_threads[0];
            t->next = nullptr;
            t->on_runqueue = false;
            publish_queue_load(e);
            if (t->state != TS_RUNNABLE_QUEUED || t->cpu_id != e.cpu_id) continue;
            if (thread_budget_available(t)) return t;
            enqueue_rt(e, t);
        }
    }
    return nullptr;
}

// Temporarily skip budget-limited nodes, then restore them. Their queue
// membership survives an idle interval and period rollover needs no remote
// heap writes or global scan to make them eligible again.
static Thread* pick_fair(Eki& e, FairPrincipalQueue& band, bool take = true) {
    const SchedBand sb = band.band;
    const u32 bi = band_index(sb);
    if (!take) {
        auto* pe = fpq_peek_min(band);
        auto* t = pe ? ftq_peek_min(band_threads(*pe, sb)) : nullptr;
        if (t && t->state == TS_RUNNABLE_QUEUED && t->cpu_id == e.cpu_id &&
            thread_budget_available(t)) return t;
    }
    PrincipalEntity* skipped[kMaxPrincipals];
    u32 skipped_count = 0;
    Thread* selected = nullptr;
    while (band.size) {
        auto* pe = fpq_pop_min(band);
        if (!pe) break;
        auto& threads = band_threads(*pe, sb);
        if (!threads.size) { pe->in_queue[bi] = false; pe->runnable[bi] = 0; continue; }
        Thread* deferred[kFairQueueCap];
        u32 deferred_count = 0;
        if (!principal_throttled(pe->principal_id)) {
            while (threads.size) {
                auto* t = ftq_pop_min(threads);
                if (t->state != TS_RUNNABLE_QUEUED || t->cpu_id != e.cpu_id) {
                    if (pe->runnable[bi]) --pe->runnable[bi];
                    if (e.queued_threads[bi]) --e.queued_threads[bi];
                    if (t->owner) __atomic_sub_fetch(&t->owner->runnable_threads, 1u, __ATOMIC_RELAXED);
                    continue;
                }
                if (!thread_budget_available(t)) { deferred[deferred_count++] = t; continue; }
                selected = t;
                if (!take) { deferred[deferred_count++] = t; }
                else {
                    if (pe->runnable[bi]) --pe->runnable[bi];
                    if (e.queued_threads[bi]) --e.queued_threads[bi];
                    if (t->owner) __atomic_sub_fetch(&t->owner->runnable_threads, 1u, __ATOMIC_RELAXED);
                }
                break;
            }
        }
        for (u32 i = 0; i < deferred_count; ++i) {
            ANKO_ASSERT(ftq_push(threads, deferred[i]));
            deferred[i]->on_runqueue = true;
        }
        if (threads.size) skipped[skipped_count++] = pe;
        else pe->in_queue[bi] = false;
        if (selected) break;
    }
    for (u32 i = 0; i < skipped_count; ++i) ANKO_ASSERT(fpq_push(band, skipped[i]));
    publish_queue_load(e);
    if (selected && take) {
        const auto& pe = e.principal_entities[selected->principal_id];
        u64& watermark = min_vruntime_for(e, sb);
        if (pe.vruntime[bi] > watermark) watermark = pe.vruntime[bi];
        // The selected principal has the least eligible service clock. This
        // floor bounds sleeper credit without mixing thread and principal time.
        if (pe.service_vruntime_ns > e.min_service_vruntime_ns)
            e.min_service_vruntime_ns = pe.service_vruntime_ns;
    }
    return selected;
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
        ANKO_ASSERT(__atomic_load_n(&t->execution_cpu, __ATOMIC_ACQUIRE) == kInvalidCpu);
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
void pending_push(Eki& target, Thread* t) {
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
    const u32 n   = kMaxCpus;
    for (u32 i = 0; i < n; ++i) {
        if (!(allowed & (1ull << i))) continue;
        const u32 load = __atomic_load_n(&g_eki[i].published_load, __ATOMIC_ACQUIRE);
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
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&waiter->lifecycle_lock));

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
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    if (t->terminate_requested || t->state == TS_RUNNING || t->on_runqueue ||
        t->state == TS_RUNNABLE_PENDING_REMOTE) return;
    t->last_wake_reason = reason;
    t->last_wake_ns     = sched_clock_ns();

    // Only doorbell and IRQ wakes get credit.  The credit remains non-transferable
    // per thread and is charged away as the receiver's principal runs.
    if (reason != WAKE_DOORBELL && reason != WAKE_IRQ) return;

    if (principal_throttled(t->principal_id)) return;

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
    if (cur->sched.sched_class == SCHED_BAND_RT_NATIVE) {
        if (woken->sched.sched_class == SCHED_BAND_RT_NATIVE &&
            woken->sched.base_priority > cur->sched.base_priority) request_switch(true, SWITCH_WAKE);
        return;
    }

    const u32 wbi = band_rank(woken->sched.sched_class);
    const u32 cbi = band_rank(cur->sched.sched_class);

    // Pick-order preemption: a higher band woke while a lower band runs.
    if (wbi < cbi && thread_budget_available(woken)) {
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

// Publish a wake while holding the thread lifecycle lock. Affinity is checked
// again here because a blocked thread may have been rebound since CPU selection.
static void wake_locked(Thread* t, u32 target_cpu) {
    Eki& source = current_eki();
    const ThreadState old = t->state;
    if (old == TS_DEAD || old == TS_RUNNING || old == TS_RUNNABLE_QUEUED ||
        old == TS_RUNNABLE_PENDING_REMOTE || t->on_runqueue || t->terminate_requested) return;
    const u64 allowed = t->affinity_mask ? t->affinity_mask & online_mask() : online_mask();
    if (!allowed) return;
    if (target_cpu >= kMaxCpus || !(allowed & (1ull << target_cpu)))
        target_cpu = first_allowed_cpu(allowed);
    if (__atomic_load_n(&t->execution_cpu, __ATOMIC_ACQUIRE) != kInvalidCpu) {
        t->deferred_wake_cpu = target_cpu;
        return;
    }
    if (target_cpu != source.cpu_id) {
        t->state = TS_RUNNABLE_PENDING_REMOTE;
        t->cpu_id = target_cpu;
        pending_push(eki_for_cpu(target_cpu), t);
        metric_inc(source.remote_wakes);
        smp::send_wake_ipi(target_cpu);
        return;
    }
    t->state = TS_RUNNABLE_QUEUED;
    t->cpu_id = source.cpu_id;
    bool enqueued;
    if (t->sched.sched_class == SCHED_BAND_RT_NATIVE) {
        enqueue_rt(source, t);
        enqueued = t->on_runqueue;
    } else enqueued = enqueue_fair(source, t);
    metric_inc(source.local_wakes);
    if (t->last_wake_reason == WAKE_DOORBELL) metric_inc(source.wake_doorbell);
    else if (t->last_wake_reason == WAKE_RESUME) metric_inc(source.wake_resume);
    if (enqueued) maybe_request_preemption_on_wake(source, t);
    send_event();
}

void wake_local(Thread* t) {
    if (!t) return;
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    wake_locked(t, current_cpu_id());
}

void wake_remote(Thread* t, u32 target_cpu) {
    if (!t) return;
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    wake_locked(t, target_cpu);
}

// ---------------------------------------------------------------------------
// Backward-compatible wake (no IPC hints) — for resume, timeout, etc.
// ---------------------------------------------------------------------------
void wake(Thread* t) {
    if (!t) return;
    const ThreadState s = t->state;
    if (s == TS_DEAD || s == TS_RUNNING || s == TS_RUNNABLE_QUEUED
        || s == TS_RUNNABLE_PENDING_REMOTE) {
        return;
    }
    apply_wake_metadata(t, WAKE_NOTIFY, IPC_HINT_NONE);
    u32 tgt;
    {
        kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
        tgt = target_cpu_for(t);
    }
    if (tgt == current_cpu_id()) wake_local(t);
    else                         wake_remote(t, tgt);
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
            handoff_forget(e, t);
            t->state = TS_DEAD;
        }
        if (!e.current && t->owner && active_ttbr0() == t->owner->ttbr0)
            mm::vm_activate(0);
        handoff_abort_reply(t);
        finish_thread(t);
    }
}

// ---------------------------------------------------------------------------
// Top-level pick
// ---------------------------------------------------------------------------
static Thread* pick_next(Eki& e) {
    drain_pending_wakes(e);
    handle_pending_steal_requests(e);

    if (Thread* t = pick_rt(e)) return t;
    Thread* next = nullptr;
    FairPrincipalQueue* chosen = nullptr;
    for (u32 b = 1; b < SCHED_BAND_IDLE; ++b) {
        auto& queue = band_for(e, static_cast<SchedBand>(b));
        auto* candidate = pick_fair(e, queue, false);
        if (!candidate) continue;
        bool better = !next;
        if (next) {
            const auto& cp = e.principal_entities[candidate->principal_id];
            const auto& np = e.principal_entities[next->principal_id];
            if (cp.service_vruntime_ns != np.service_vruntime_ns)
                better = cp.service_vruntime_ns < np.service_vruntime_ns;
            else if (candidate->principal_id == next->principal_id && effective_key(candidate) != effective_key(next))
                better = effective_key(candidate) < effective_key(next);
            else better = band_rank(candidate->sched.sched_class) < band_rank(next->sched.sched_class);
        }
        if (better) { next = candidate; chosen = &queue; }
    }
    if (chosen) return pick_fair(e, *chosen);
    return pick_fair(e, e.idle_principals);
}

// ---------------------------------------------------------------------------
// EL0 entry (initial first-run path)
// ---------------------------------------------------------------------------
extern "C" [[noreturn]] void enter_user_frame(const TrapFrame* frame);

static u64 active_ttbr0() {
    u64 root;
    asm volatile("mrs %0, TTBR0_EL1" : "=r"(root));
    return root;
}

static void activate_thread_space(Thread* t) {
    if (!t) return;
    if (t->owner) {
        if (active_ttbr0() != t->owner->ttbr0) mm::vm_activate(t->owner->ttbr0);
    }
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
    g_cntfrq = platform::counter_frequency(g_cntfrq);
    for (u32 i = 0; i < kMaxCpus; ++i) init_eki(i);

    principal_init_bootstrap();

    // Mirror weights into per-CPU entities.
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) principal_mirror_weights(cpu);

    __atomic_store_n(&g_online_cpu_mask, 1ull, __ATOMIC_RELEASE);
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
        __atomic_store_n(&next->execution_cpu, e.cpu_id, __ATOMIC_RELEASE);
        metric_inc(e.context_switches);
        metric_inc(next->switches);
        next->last_start_ns = sched_clock_ns();
        next->slice_start_ns = next->last_start_ns;
        activate_thread_space(next);
        ANKO_LOGF_INFO("kernel/sched",
                       "dispatch tid=%u pid=%u elr=%llx",
                       next->tid, next->pid,
                       static_cast<unsigned long long>(next->tf.elr));
        enter_user_frame(&next->tf);
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
    // Timer interrupts also wake a CPU whose only queued work is budget-limited.
    send_event();
    if (!cur) return;
    const u64 now = sched_clock_ns();
    const u64 ran_in_slice = (cur->slice_start_ns && now >= cur->slice_start_ns)
        ? now - cur->slice_start_ns : 0;
    const bool handoff_expired = handoff_deadline_expired(cur, now);
    account_current(e);

    if (e.switch_pending) return;
    if ((cur->slice_ns > 0 && ran_in_slice >= cur->slice_ns) ||
        handoff_expired) {
        metric_inc(e.timer_preempts);
        request_switch(true, SWITCH_TIMER);
    }
}

// ---------------------------------------------------------------------------
// Yield / block
// ---------------------------------------------------------------------------
void yield() {
    note_yield();
    request_switch(true, SWITCH_YIELD);
}

void block(Thread* t) {
    if (!t) return;
    if (t == current_eki().current) account_current(current_eki());
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    t->in_drain_quantum = false;
    t->drain_quantum_start_ns = 0;
    if (t->state != TS_SLEEPING) t->state = TS_BLOCKED;
    if (t == current_eki().current) request_switch(false, SWITCH_BLOCK);
}

bool switch_pending() {
    return current_eki().switch_pending;
}

// ---------------------------------------------------------------------------
// Enqueue public API
// ---------------------------------------------------------------------------
void enqueue(Thread* t) {
    if (!t) return;
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    if (t->terminate_requested || t->execution_cpu != kInvalidCpu) return;
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
        || reason == SWITCH_HANDOFF) {
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
    if (!frame_ptr || !cur) return;
    collect_terminations(e, cur);
    auto* frame = static_cast<TrapFrame*>(frame_ptr);
    cur->tf = *frame;
    if (e.switch_pending || cur->state != TS_RUNNING ||
        __atomic_load_n(&cur->terminate_requested, __ATOMIC_ACQUIRE))
        account_current(e);
    if (__atomic_load_n(&cur->terminate_requested, __ATOMIC_ACQUIRE)) cur->state = TS_DEAD;
    if (!e.switch_pending && cur->state != TS_RUNNING)
        request_switch(false, cur->state == TS_DEAD ? SWITCH_EXIT : SWITCH_BLOCK);
    if (!e.switch_pending) { activate_thread_space(cur); return; }
    // Retain only on the switch path. After remote publication this CPU uses
    // the pointer only for identity comparison, unless it owns the retirement.
    object::Ref<Thread> current_reference(cur);
    const bool retiring_current = cur->state == TS_DEAD;
    const usize previous_address = reinterpret_cast<usize>(cur);

    const bool requeue = e.requeue_current;
    const SwitchReason reason = e.pending_reason;
    e.switch_pending = false;
    e.requeue_current = false;
    e.pending_reason = SWITCH_NONE;
    Thread* next = handoff_take_next(e);
    {
        kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&cur->lifecycle_lock));
        if (requeue && cur->state == TS_RUNNING) {
            cur->state = TS_RUNNABLE_QUEUED;
            if (cur->sched.sched_class == SCHED_BAND_RT_NATIVE) enqueue_rt(e, cur);
            else enqueue_fair(e, cur);
        }
        e.current = nullptr;
        __atomic_store_n(&cur->execution_cpu, kInvalidCpu, __ATOMIC_RELEASE);
        const u32 wake_cpu = cur->deferred_wake_cpu;
        cur->deferred_wake_cpu = kInvalidCpu;
        if (wake_cpu != kInvalidCpu && !cur->terminate_requested &&
            (cur->state == TS_BLOCKED || cur->state == TS_SLEEPING)) {
            if (wake_cpu == e.cpu_id) {
                cur->state = TS_RUNNABLE_QUEUED;
                if (cur->sched.sched_class == SCHED_BAND_RT_NATIVE) enqueue_rt(e, cur);
                else enqueue_fair(e, cur);
            } else {
                cur->state = TS_RUNNABLE_PENDING_REMOTE;
                cur->cpu_id = wake_cpu;
                pending_push(eki_for_cpu(wake_cpu), cur);
                smp::send_wake_ipi(wake_cpu);
            }
        }
    }
    if (!retiring_current) {
        // A sleeping thread can wake, migrate, and die while this CPU is idle.
        // The saved frame is no longer ours; do not pin its process in WFE.
        current_reference.reset();
        cur = nullptr;
    }
    // Direct handoff must obey the same eligibility and timing contracts as
    // queue dispatch. The receiver remains sleeping until this point.
    if (next) {
        if (__atomic_load_n(&next->terminate_requested, __ATOMIC_ACQUIRE) ||
            next->execution_cpu != kInvalidCpu || !thread_budget_available(next)) {
            if (!next->terminate_requested) wake_local(next);
            next = nullptr;
        } else {
            next->state = TS_RUNNABLE_QUEUED;
            next->slice_ns = compute_slice_ns(next, 1);
            next->handoff_deadline_ns = sched_clock_ns() + kIpcDrainQuantumNs;
        }
    }
    if (!next) next = pick_next(e);
    while (!next) {
        if (cur && retiring_current) {
            mm::vm_activate(0);
            finish_thread(cur);
            current_reference.reset();
            cur = nullptr;
        }
        maybe_send_steal_request(e);
        wait_for_interrupt();
        drain_pending_wakes(e);
        collect_terminations(e, cur);
        next = pick_next(e);
    }
    if (reinterpret_cast<usize>(next) != previous_address) {
        metric_inc(e.context_switches);
        metric_inc(next->switches);
        switch (reason) {
        case SWITCH_YIELD: metric_inc(e.yield_switches); break;
        case SWITCH_BLOCK: metric_inc(e.block_switches); break;
        case SWITCH_EXIT: metric_inc(e.exit_switches); break;
        case SWITCH_IDLE: metric_inc(e.idle_switches); break;
        case SWITCH_HANDOFF: metric_inc(e.handoff_success); break;
        default: break;
        }
    }
    {
        kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&next->lifecycle_lock));
        ANKO_ASSERT(next->state == TS_RUNNABLE_QUEUED && !next->on_runqueue);
        e.current = next;
        next->state = TS_RUNNING;
        __atomic_store_n(&next->execution_cpu, e.cpu_id, __ATOMIC_RELEASE);
        next->last_start_ns = sched_clock_ns();
        next->slice_start_ns = next->last_start_ns;
        next->last_cpu = next->cpu_id;
        next->cpu_id = e.cpu_id;
        ipc_drain_quantum_begin(next, next->last_start_ns);
    }
    activate_thread_space(next);
    *frame = next->tf;
    e.switch_pending = false;
    e.requeue_current = false;
    e.pending_reason = SWITCH_NONE;
    if (cur && cur != next && retiring_current) {
        finish_thread(cur);
        current_reference.reset();
    }
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

u32  online_cpu_count() { return __builtin_popcountll(online_mask()); }
u64  online_cpu_mask() { return online_mask(); }
void set_online_cpu_count(u32 n) {
    if (!n) n = 1;
    if (n > kMaxCpus) n = kMaxCpus;
    __atomic_store_n(&g_online_cpu_mask, (1ull << n) - 1, __ATOMIC_RELEASE);
}
void mark_cpu_online(u32 cpu_id) {
    if (cpu_id >= kMaxCpus) return;
    // Mirror principal weights to secondary CPU Eki
    principal_mirror_weights(cpu_id);
    __atomic_fetch_or(&g_online_cpu_mask, 1ull << cpu_id, __ATOMIC_RELEASE);
}
void note_ipi_sent(u32)   { metric_inc(current_eki().ipis_sent); }
void note_ipi_handled()   { metric_inc(current_eki().ipis_handled); }
void drain_pending_wakes() { drain_pending_wakes(current_eki()); }

} // namespace sched

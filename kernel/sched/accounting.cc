// kernel/sched/accounting.cc — principal accounting, budgets, and throttling.
//
// Owns the global principal table (PrincipalSchedState): per-principal runtime,
// period/budget refill, throttle state, class/weight metadata, and the
// allocation lifecycle.  account_current() charges the running thread's elapsed
// time back to thread, process, and principal before each context switch and on
// every tick.

#include "anko/sched.h"
#include "anko/spinlock.h"
#include "libk/string.h"
#include "sched_internal.h"

namespace sched {

// ---------------------------------------------------------------------------
// Principal table
// ---------------------------------------------------------------------------
static PrincipalSchedState g_principal_sched[kMaxPrincipalsLocal]{};
static u32 g_next_principal_id = 1; // principal 0 is reserved for the kernel/bootstrap bucket
static kernel::SpinLock g_principal_lock{};
struct alignas(64) PrincipalGuard { kernel::SpinLock lock; };
static PrincipalGuard g_principal_guards[kMaxPrincipalsLocal]{};
static u32 g_principal_generations[kMaxPrincipalsLocal]{};
static u32 g_principal_references[kMaxPrincipalsLocal]{};

// Entry locks serialize charging and period rollover across CPUs. Allocation
// takes the table lock before an entry lock; scheduling only takes entry locks.
static void refill(PrincipalSchedState& ps, u64 now) {
    if (!ps.budget_ns || !ps.period_ns || now < ps.period_start_ns ||
        now - ps.period_start_ns < ps.period_ns) return;
    ps.period_start_ns += ((now - ps.period_start_ns) / ps.period_ns) * ps.period_ns;
    ps.used_in_period_ns = 0;
    ps.throttled = false;
}

bool principal_allocated(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return false;
    kernel::LockGuard guard(g_principal_guards[pid].lock);
    return g_principal_sched[pid].allocated;
}

bool principal_throttled(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return true;
    kernel::LockGuard guard(g_principal_guards[pid].lock);
    auto& ps = g_principal_sched[pid];
    refill(ps, sched_clock_ns());
    return !ps.allocated || ps.throttled;
}

u32 principal_weight(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return NICE_0_WEIGHT;
    kernel::LockGuard guard(g_principal_guards[pid].lock);
    const u32 weight = g_principal_sched[pid].weight;
    return weight ? weight : NICE_0_WEIGHT;
}

u32 principal_generation(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return 0;
    kernel::LockGuard guard(g_principal_guards[pid].lock);
    return g_principal_sched[pid].generation;
}

bool thread_budget_available(const Thread* t) {
    return t && (!t->principal_generation || t->principal_generation == principal_generation(t->principal_id)) &&
        !principal_throttled(t->principal_id) && sched_context_available(t->sched_context);
}

bool retain_principal(u32 pid) {
    if (!pid) return true;
    if (pid >= kMaxPrincipalsLocal) return false;
    kernel::LockGuard guard(g_principal_guards[pid].lock);
    if (!g_principal_sched[pid].allocated) return false;
    ++g_principal_references[pid];
    return true;
}

// Reserve principal 0 for kernel/bootstrap work.  User processes and VMs get
// unique IDs; PrincipalClass is authorization metadata, not the fairness bucket.
void principal_init_bootstrap() {
    g_principal_sched[0].principal_id = 0;
    g_principal_sched[0].class_ = PRINCIPAL_KERNEL;
    g_principal_sched[0].allocated = 1;
    g_principal_sched[0].weight = NICE_0_WEIGHT;
    g_principal_sched[0].generation = 1;
    g_principal_sched[0].period_start_ns = sched_clock_ns();
}

// Mirror principal weights into one CPU's per-principal entities.
void principal_mirror_weights(u32 cpu_id) {
    Eki& e = eki_for_cpu(cpu_id);
    for (u32 i = 0; i < kMaxPrincipals; ++i)
        e.principal_entities[i].weight = principal_weight(i);
}

// ---------------------------------------------------------------------------
// Runtime accounting — charge elapsed time to thread / process / principal.
// Called before every context switch and on tick.
// ---------------------------------------------------------------------------
void account_current(Eki& e) {
    Thread* t = e.current;
    if (!t || t == e.idle) return;
    // A blocking syscall has already changed state, but still owns execution
    // until post_syscall saves its frame. Charge that last interval too.
    kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
    const u64 now = sched_clock_ns();
    const u64 delta = now - t->last_start_ns;
    t->last_start_ns = now;
    t->runtime_ns += delta;
    if (t->owner) __atomic_add_fetch(&t->owner->runtime_ns, delta, __ATOMIC_RELAXED);

    const u32 pid = t->principal_id;
    if (pid >= kMaxPrincipalsLocal) return;
    bool exhausted = false;
    {
        kernel::LockGuard guard(g_principal_guards[pid].lock);
        auto& ps = g_principal_sched[pid];
        refill(ps, now);
        ps.runtime_ns += delta;
        ps.used_in_period_ns += delta;
        exhausted = ps.budget_ns && ps.used_in_period_ns >= ps.budget_ns;
        ps.throttled = exhausted;
    }
    PrincipalEntity& pe = e.principal_entities[pid];
    pe.weight = principal_weight(pid);
    pe.runtime_ns += delta;
    const u32 bi = band_index(t->sched.sched_class);
    const u64 principal_delta = delta * NICE_0_WEIGHT / pe.weight;
    pe.vruntime[bi] += principal_delta;
    pe.service_vruntime_ns += principal_delta;
    // A principal may have threads in several bands. Every heap containing it
    // uses the same service clock, so all of those positions need repair.
    for (u32 b = 1; b < NUM_BANDS; ++b)
        if (pe.in_queue[b]) fpq_resift(band_for(e, static_cast<SchedBand>(b)), &pe);

    const u32 tw = t->sched.weight ? t->sched.weight : NICE_0_WEIGHT;
    t->vruntime_ns += delta * NICE_0_WEIGHT / tw;
    u64 minimum = t->vruntime_ns;
    for (u32 b = 1; b < NUM_BANDS; ++b) {
        if (auto* queued = ftq_peek_min(band_threads(pe, static_cast<SchedBand>(b)))) {
            const u64 key = effective_key(queued);
            if (key < minimum) minimum = key;
        }
    }
    if (minimum > pe.thread_min_vruntime_ns) pe.thread_min_vruntime_ns = minimum;

    const u64 credit = t->wake_credit_ns > 0 ? static_cast<u64>(t->wake_credit_ns) : 0;
    t->wake_credit_ns = static_cast<i64>(credit > delta ? credit - delta : 0);
    ipc_drain_quantum_expire(t, now);
    if (charge_sched_context(t->sched_context, delta, now) || exhausted)
        request_switch(true, SWITCH_BUDGET);
}

// ---------------------------------------------------------------------------
// Explicit bulk refill for diagnostics. Dispatch refills only the principal it
// examines, including when the station has spent a whole period idle.
// ---------------------------------------------------------------------------
void refill_principal_budgets() {
    const u64 now = sched_clock_ns();
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        kernel::LockGuard guard(g_principal_guards[i].lock);
        refill(g_principal_sched[i], now);
    }
}

// ---------------------------------------------------------------------------
// Principal allocation lifecycle
// ---------------------------------------------------------------------------
u32 allocate_principal(PrincipalClass class_, u32 weight, u64 budget_ns, u64 period_ns) {
    if (budget_ns && (!period_ns || budget_ns > period_ns)) return 0;
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
    kernel::LockGuard entry(g_principal_guards[id].lock);
    PrincipalSchedState& ps = g_principal_sched[id];
    libk::memset(&ps, 0, sizeof(ps));
    ps.principal_id = id;
    ps.class_ = class_;
    ps.allocated = 1;
    g_principal_references[id] = 1;
    ps.generation = ++g_principal_generations[id];
    if (!ps.generation) ps.generation = ++g_principal_generations[id];
    ps.weight = weight ? weight : NICE_0_WEIGHT;
    ps.budget_ns = budget_ns;
    ps.period_ns = period_ns;
    ps.period_start_ns = sched_clock_ns();
    return id;
}

void release_principal(u32 id) {
    if (id == 0 || id >= kMaxPrincipalsLocal) return;
    kernel::LockGuard guard(g_principal_lock);
    kernel::LockGuard entry(g_principal_guards[id].lock);
    if (!g_principal_references[id] || --g_principal_references[id]) return;
    g_principal_sched[id] = {};
    if (id < g_next_principal_id) g_next_principal_id = id;
    // Per-CPU entities belong to their CPU. Enqueue notices a new generation
    // and resets an empty local entity; teardown never clears a peer's heaps.
}

// ---------------------------------------------------------------------------
// Principal runtime / fairness accessors
// ---------------------------------------------------------------------------
u64 principal_runtime(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return 0;
    kernel::LockGuard guard(g_principal_guards[pid].lock);
    return g_principal_sched[pid].runtime_ns;
}

u64 principal_class_runtime(PrincipalClass class_) {
    u64 total = 0;
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        kernel::LockGuard guard(g_principal_guards[i].lock);
        PrincipalSchedState& ps = g_principal_sched[i];
        if (ps.allocated && ps.class_ == class_) {
            total += ps.runtime_ns;
        }
    }
    return total;
}

u64 fairness_max_lag() {
    u64 min_v = ~0ull, max_v = 0;
    bool any = false;
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        const u64 rt = principal_runtime(i);
        if (!rt) continue;
        any = true;
        if (rt < min_v) min_v = rt;
        if (rt > max_v) max_v = rt;
    }
    return any ? (max_v - min_v) : 0;
}

} // namespace sched

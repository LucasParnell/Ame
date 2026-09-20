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

bool principal_allocated(u32 pid) {
    return pid < kMaxPrincipalsLocal && g_principal_sched[pid].allocated;
}

bool principal_throttled(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return false;
    return __atomic_load_n(&g_principal_sched[pid].throttled, __ATOMIC_RELAXED);
}

// Effective weight for a principal, falling back to NICE_0_WEIGHT when the slot
// is out of range or its weight was never set.
u32 principal_weight(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return NICE_0_WEIGHT;
    const u32 w = g_principal_sched[pid].weight;
    return w ? w : NICE_0_WEIGHT;
}

// Reserve principal 0 for kernel/bootstrap work.  User processes and VMs get
// unique IDs; PrincipalClass is authorization metadata, not the fairness bucket.
void principal_init_bootstrap() {
    g_principal_sched[0].principal_id = 0;
    g_principal_sched[0].class_ = PRINCIPAL_KERNEL;
    g_principal_sched[0].allocated = 1;
    g_principal_sched[0].weight = NICE_0_WEIGHT;
    g_principal_sched[0].period_start_ns = sched_clock_ns();
}

// Mirror principal weights into one CPU's per-principal entities.
void principal_mirror_weights(u32 cpu_id) {
    Eki& e = eki_for_cpu(cpu_id);
    for (u32 i = 0; i < kMaxPrincipals; ++i)
        e.principal_entities[i].weight = g_principal_sched[i].weight;
}

// ---------------------------------------------------------------------------
// Runtime accounting — charge elapsed time to thread / process / principal.
// Called before every context switch and on tick.
// ---------------------------------------------------------------------------
void account_current(Eki& e) {
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

        // IPC drain quantum expiry: strip credit if went CPU-bound.
        ipc_drain_quantum_expire(t, now);

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
void refill_principal_budgets() {
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
                if (i < kMaxPrincipals) eki_for_cpu(cpu).principal_entities[i].throttled = false;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Principal allocation lifecycle
// ---------------------------------------------------------------------------
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
        eki_for_cpu(cpu).principal_entities[id].weight = ps.weight;
    return id;
}

void release_principal(u32 id) {
    if (id == 0 || id >= kMaxPrincipalsLocal) return;
    kernel::LockGuard guard(g_principal_lock);
    libk::memset(&g_principal_sched[id], 0, sizeof(g_principal_sched[id]));
    if (id < g_next_principal_id) g_next_principal_id = id;
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu) {
        PrincipalEntity& pe = eki_for_cpu(cpu).principal_entities[id];
        libk::memset(&pe, 0, sizeof(pe));
        pe.principal_id = id;
        for (u32 b = 0; b < NUM_BANDS; ++b) pe.heap_idx[b] = 0xff;
    }
}

// ---------------------------------------------------------------------------
// Principal runtime / fairness accessors
// ---------------------------------------------------------------------------
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

} // namespace sched

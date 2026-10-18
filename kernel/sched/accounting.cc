// kernel/sched/accounting.cc — principal accounting, budgets, and throttling.
//
// Owns the published principal policy, CPU-owned runtime columns,
// atomic period/budget windows, class/weight metadata, and the
// allocation lifecycle.  account_current() charges the running thread's elapsed
// time back to thread, process, and principal before each context switch and on
// every tick.

#include "anko/sched.h"
#include "anko/spinlock.h"
#include "sched_internal.h"

namespace sched {

// ---------------------------------------------------------------------------
// Principal table
// ---------------------------------------------------------------------------
static PrincipalSchedState g_principal_sched[kMaxPrincipalsLocal]{};

// Separate from PrincipalSchedState so its 64-byte layout contract is intact.
struct IpcPolicyCell {
    u64 parks;
    u64 wakes;
    u32 budget;   // 0 = never spin
    u32 generation;
};
static IpcPolicyCell g_ipc_policy[kMaxPrincipalsLocal]{};
static u32 g_next_principal_id = 1; // principal 0 is reserved for the kernel/bootstrap bucket
static kernel::SpinLock g_principal_lock{};
static u32 g_principal_generations[kMaxPrincipalsLocal]{};
static u32 g_principal_references[kMaxPrincipalsLocal]{};

// One writer per CPU; diagnostics sum these columns only when requested.
// Principals shared across CPUs no longer bounce one runtime cache line on
// every tick. Relaxed stores/loads permit concurrent diagnostics without a
// read-modify-write on the charging path. Lifecycle clears a column only after
// the final process/context reference has gone, so it has no charging writer.
struct alignas(64) CpuRuntime { u64 principal[kMaxPrincipalsLocal]; u64 busy_ns; };
static CpuRuntime g_runtime[kMaxCpus]{};

static u64 published_policy(u32 pid) {
    return pid < kMaxPrincipalsLocal
        ? __atomic_load_n(&g_principal_sched[pid].dispatch, __ATOMIC_ACQUIRE) : 0;
}

bool principal_allocated(u32 pid) {
    return published_policy(pid) != 0;
}

bool principal_throttled(u32 pid) {
    return !principal_dispatch(pid).available;
}

PrincipalDispatch principal_dispatch(u32 pid) {
    const u64 policy = published_policy(pid);
    if (!policy) return {0, NICE_0_WEIGHT, false};
    auto& ps = g_principal_sched[pid];
    // The caller owns a process/context reference for this ID, keeping policy
    // immutable. Unbudgeted work needs neither a clock read nor a shared RMW.
    const bool available = !ps.budget_ns || ps.budget_window.charge(
        0, sched_clock_ns(), ps.period_ns).used_ns < ps.budget_ns;
    return {static_cast<u32>(policy >> 32), static_cast<u32>(policy), available};
}

u32 principal_weight(u32 pid) {
    const u32 weight = static_cast<u32>(published_policy(pid));
    return weight ? weight : NICE_0_WEIGHT;
}

u32 principal_generation(u32 pid) {
    return static_cast<u32>(published_policy(pid) >> 32);
}

bool thread_budget_available(const Thread* t) {
    if (!t) return false;
    const auto policy = principal_dispatch(t->principal_id);
    return (!t->principal_generation || t->principal_generation == policy.generation) &&
        policy.available && sched_context_available(t->sched_context);
}

bool retain_principal(u32 pid) {
    if (!pid) return true;
    if (pid >= kMaxPrincipalsLocal) return false;
    kernel::LockGuard guard(g_principal_lock);
    if (!principal_allocated(pid)) return false;
    ++g_principal_references[pid];
    return true;
}

// Reserve principal 0 for kernel/bootstrap work.  User processes and VMs get
// unique IDs; PrincipalClass is authorization metadata, not the fairness bucket.
void principal_init_bootstrap() {
    auto& ps = g_principal_sched[0];
    ps.class_ = PRINCIPAL_KERNEL;
    ps.budget_window.reset(sched_clock_ns());
    for (auto& cpu : g_runtime) __atomic_store_n(&cpu.principal[0], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ps.dispatch, (1ull << 32) | NICE_0_WEIGHT, __ATOMIC_RELEASE);
}

// Mirror principal weights into one CPU's per-principal entities.
void ipc_note_park(u32 id) {
    if (id >= kMaxPrincipalsLocal) return;
    __atomic_add_fetch(&g_ipc_policy[id].parks, 1, __ATOMIC_RELAXED);
}

void ipc_note_wake(u32 id) {
    if (id >= kMaxPrincipalsLocal) return;
    __atomic_add_fetch(&g_ipc_policy[id].wakes, 1, __ATOMIC_RELAXED);
}

u32 ipc_poll_budget(u32 id) {
    return id < kMaxPrincipalsLocal ? __atomic_load_n(&g_ipc_policy[id].budget, __ATOMIC_RELAXED) : 0;
}

u32 ipc_poll_budget_set(u32 id, u32 budget) {
    if (id >= kMaxPrincipalsLocal) return 0;
    if (budget > kIpcPollBudgetMax) budget = kIpcPollBudgetMax;
    __atomic_store_n(&g_ipc_policy[id].budget, budget, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ipc_policy[id].generation, 1, __ATOMIC_RELAXED);
    return budget;
}

u64 ipc_park_count(u32 id) {
    return id < kMaxPrincipalsLocal ? __atomic_load_n(&g_ipc_policy[id].parks, __ATOMIC_RELAXED) : 0;
}

u64 ipc_wake_count(u32 id) {
    return id < kMaxPrincipalsLocal ? __atomic_load_n(&g_ipc_policy[id].wakes, __ATOMIC_RELAXED) : 0;
}

void principal_mirror_weights(u32 cpu_id) {
    Eki& e = eki_for_cpu(cpu_id);
    for (u32 i = 0; i < kMaxPrincipals; ++i)
        e.principal_entities[i].weight = principal_weight(i);
}

// ---------------------------------------------------------------------------
// Runtime accounting — charge elapsed time to thread / process / principal.
// Called before every context switch and on tick.
// ---------------------------------------------------------------------------
static void account_owned(Eki& e, Thread* t, u64 now) {
    const u64 delta = now - t->last_start_ns;
    t->last_start_ns = now;
    t->runtime_ns += delta;
    auto& busy = g_runtime[e.cpu_id].busy_ns;
    __atomic_store_n(&busy, __atomic_load_n(&busy, __ATOMIC_RELAXED) + delta, __ATOMIC_RELAXED);
    if (t->owner) __atomic_add_fetch(&t->owner->runtime_ns, delta, __ATOMIC_RELAXED);

    const u32 pid = t->principal_id;
    if (pid >= kMaxPrincipalsLocal) return;
    auto& runtime = g_runtime[e.cpu_id].principal[pid];
    __atomic_store_n(&runtime, __atomic_load_n(&runtime, __ATOMIC_RELAXED) + delta,
                     __ATOMIC_RELAXED);
    auto& ps = g_principal_sched[pid];
    const bool exhausted = ps.budget_ns && ps.budget_window.charge(
        delta, now, ps.period_ns).used_ns >= ps.budget_ns;
    PrincipalEntity& pe = e.principal_entities[pid];
    // A queued/running thread pins immutable principal policy. Reuse the
    // station's generation-checked weight instead of rereading a shared line.
    if (!t->principal_generation || pe.generation != t->principal_generation)
        pe.weight = principal_weight(pid);
    const u64 principal_delta = pe.weight == NICE_0_WEIGHT ? delta : delta * NICE_0_WEIGHT / pe.weight;
    // This shared clock orders every band containing the principal. Legacy
    // per-band clocks and a second runtime total had no selection/metric reader;
    // keep the authoritative totals above and the clocks used by the picker.
    pe.service_vruntime_ns += principal_delta;
    // A principal may have threads in several bands. Every heap containing it
    // uses the same service clock, so all of those positions need repair.
    for (u32 b = 1; b < NUM_BANDS; ++b)
        if (pe.in_queue[b]) fpq_resift(band_for(e, static_cast<SchedBand>(b)), &pe);

    const u32 tw = t->sched.weight ? t->sched.weight : NICE_0_WEIGHT;
    t->vruntime_ns += tw == NICE_0_WEIGHT ? delta : delta * NICE_0_WEIGHT / tw;
    u64 minimum = t->vruntime_ns;
    for (u32 b = 1; b < NUM_BANDS; ++b) {
        if (auto* queued = ftq_peek_min(band_threads(e, pe, static_cast<SchedBand>(b)))) {
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

static void account_current_owned_at(Eki& e, u64 now) {
    Thread* t = e.current;
    if (!t || t == e.idle) return;
    if (t->state == TS_RUNNING) {
        // Wakers and context binders cannot change a running thread. Its owner
        // can charge without acquiring a lock that those remote paths may hold.
        account_owned(e, t, now);
    } else {
        // Blocking has exposed wake metadata before the frame is published.
        // Serialize the final charge with that exceptional transition.
        kernel::LockGuard life(*reinterpret_cast<kernel::SpinLock*>(&t->lifecycle_lock));
        account_owned(e, t, now);
    }
}

void account_current_at(Eki& e, u64 now) {
    kernel::InterruptGuard irq;
    account_current_owned_at(e, now);
}

void account_current(Eki& e) {
    // Take the timestamp inside IRQ exclusion so an interrupt cannot charge a
    // newer interval before this call applies an older sample.
    kernel::InterruptGuard irq;
    account_current_owned_at(e, sched_clock_ns());
}

// ---------------------------------------------------------------------------
// Explicit bulk refill for diagnostics. Dispatch refills only the principal it
// examines, including when the station has spent a whole period idle.
// ---------------------------------------------------------------------------
void refill_principal_budgets() {
    kernel::LockGuard guard(g_principal_lock);
    const u64 now = sched_clock_ns();
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        auto& ps = g_principal_sched[i];
        if (principal_allocated(i) && ps.budget_ns)
            ps.budget_window.charge(0, now, ps.period_ns);
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
        if (!principal_allocated(candidate)) {
            id = candidate;
            g_next_principal_id = (candidate % (kMaxPrincipalsLocal - 1)) + 1;
            break;
        }
    }
    if (id == 0) return 0;
    PrincipalSchedState& ps = g_principal_sched[id];
    ps.class_ = class_;
    g_principal_references[id] = 1;
    u32 generation = ++g_principal_generations[id];
    if (!generation) generation = ++g_principal_generations[id];
    ps.budget_ns = budget_ns;
    ps.period_ns = period_ns;
    ps.budget_window.reset(sched_clock_ns());
    for (auto& cpu : g_runtime) __atomic_store_n(&cpu.principal[id], 0, __ATOMIC_RELAXED);
    const u64 policy = static_cast<u64>(generation) << 32 | (weight ? weight : NICE_0_WEIGHT);
    __atomic_store_n(&ps.dispatch, policy, __ATOMIC_RELEASE);
    return id;
}

void release_principal(u32 id) {
    if (id == 0 || id >= kMaxPrincipalsLocal) return;
    kernel::LockGuard guard(g_principal_lock);
    if (!g_principal_references[id] || --g_principal_references[id]) return;
    __atomic_store_n(&g_principal_sched[id].dispatch, 0, __ATOMIC_RELEASE);
    if (id < g_next_principal_id) g_next_principal_id = id;
    // Per-CPU entities belong to their CPU. Enqueue notices a new generation
    // and resets an empty local entity; teardown never clears a peer's heaps.
}

// ---------------------------------------------------------------------------
// Principal runtime / fairness accessors
// ---------------------------------------------------------------------------
u64 busy_runtime_ns() {
    u64 total = 0;
    for (auto& cpu : g_runtime)
        total += __atomic_load_n(&cpu.busy_ns, __ATOMIC_RELAXED);
    return total;
}

u64 principal_runtime(u32 pid) {
    if (pid >= kMaxPrincipalsLocal) return 0;
    const u64 policy = published_policy(pid);
    if (!policy) return 0;
    u64 total = 0;
    for (auto& cpu : g_runtime)
        total += __atomic_load_n(&cpu.principal[pid], __ATOMIC_RELAXED);
    // Diagnostics must not mix counters from two lifetimes of a recycled ID.
    return published_policy(pid) == policy ? total : 0;
}

u64 principal_class_runtime(PrincipalClass class_) {
    kernel::LockGuard guard(g_principal_lock);
    u64 total = 0;
    for (u32 i = 0; i < kMaxPrincipalsLocal; ++i) {
        PrincipalSchedState& ps = g_principal_sched[i];
        if (principal_allocated(i) && ps.class_ == class_) {
            total += principal_runtime(i);
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

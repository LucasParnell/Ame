// kernel/sched/sched_context.cc — SchedContext table and lifecycle.
//
// Owns the SchedContext pool and its guards: creation from a caller-checked
// request, cap binding, per-context budget refill, and disposal. Accounting
// that references a context's budget is kept next to the table it reads.

#include "anko/sched.h"
#include "anko/object.h"
#include "anko/panic.h"
#include "anko/spinlock.h"
#include "sched_internal.h"

namespace sched {

constexpr u32 kMaxSchedContexts = 128;
static SchedContext g_sched_contexts[kMaxSchedContexts]{};
static bool g_sched_context_allocated[kMaxSchedContexts]{};
static kernel::SpinLock g_context_lock{};
static u32 g_next_sched_context_id = 1;

void refill_sched_context_budgets() {
    kernel::LockGuard guard(g_context_lock);
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

object::Ref<SchedContext> sched_context_by_id(u32 id) {
    kernel::LockGuard guard(g_context_lock);
    if (id == 0) return nullptr;
    for (u32 i = 0; i < kMaxSchedContexts; ++i) {
        if (g_sched_context_allocated[i] && g_sched_contexts[i].id == id) return object::Ref<SchedContext>(&g_sched_contexts[i]);
    }
    return nullptr;
}

cap::Borrow<SchedContext> sched_context_from_cap(Process& caller, CapHandle sched_context_cap, SysError* out_error) {
    cap::Ref e = cap::lookup(*caller.cap_table, sched_context_cap, CAP_SCHED_CONTEXT);
    if (!e) { if (out_error) *out_error = E_NOTFOUND; return nullptr; }
    if ((e->rights_mask & RIGHT_SCHED_CONTEXT_CONTROL) == 0) {
        if (out_error) *out_error = E_PERM;
        return nullptr;
    }
    auto* ctx = static_cast<SchedContext*>(e->target);
    if (!ctx) { if (out_error) *out_error = E_NOTFOUND; return nullptr; }
    if (out_error) *out_error = E_OK;
    return cap::Borrow<SchedContext>(e);
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
    if (ctx.principal_id >= kMaxPrincipals || !principal_allocated(ctx.principal_id))
        return false;
    return true;
}

static void dispose_sched_context(void* target) {
    kernel::LockGuard guard(g_context_lock);
    auto* ctx = static_cast<SchedContext*>(target);
    const usize slot = ctx - g_sched_contexts;
    ANKO_ASSERT(slot < kMaxSchedContexts);
    g_sched_context_allocated[slot] = false;
    *ctx = {};
}

CapHandle sched_context_create(Process& caller, const SchedContext& request, SysError* out_error) {
    SchedContext ctx = request;
    if (!sanitize_context_for_caller(caller, ctx)) {
        if (out_error) *out_error = E_INVALID;
        return kNullCap;
    }
    u32 slot = kMaxSchedContexts;
    {
        kernel::LockGuard guard(g_context_lock);
        for (u32 i = 0; i < kMaxSchedContexts; ++i) if (!g_sched_context_allocated[i]) { slot = i; break; }
        if (slot == kMaxSchedContexts) { if (out_error) *out_error = E_OVERFLOW; return kNullCap; }
        ctx.id = g_next_sched_context_id++; ctx.used_ns = 0; ctx.period_start_ns = sched_clock_ns();
        g_sched_contexts[slot] = ctx; g_sched_context_allocated[slot] = true;
    }
    auto* root = object::create(&g_sched_contexts[slot], dispose_sched_context);
    if (!root) { dispose_sched_context(&g_sched_contexts[slot]); if (out_error) *out_error = E_NOMEM; return kNullCap; }
    CapHandle h = cap::alloc(*caller.cap_table, CAP_SCHED_CONTEXT, &g_sched_contexts[slot],
                            RIGHT_READ | RIGHT_DUP | RIGHT_SCHED_CONTEXT_CONTROL);
    object::release(root);
    if (out_error) *out_error = h ? E_OK : E_OVERFLOW;
    return h;
}

static bool bind_thread_to_context(Thread* t, SchedContext* ctx) {
    if (!t || !ctx) return false;
    if (t->state == TS_DEAD || t->on_runqueue || t->state == TS_RUNNING ||
        t->state == TS_RUNNABLE_PENDING_REMOTE || t->state == TS_RUNNABLE_QUEUED) return false;
    t->principal_id = ctx->principal_id;
    t->sched_context_id = ctx->id;
    auto* next_context = object::acquire(ctx);
    object::release(t->sched_context_lifetime); t->sched_context_lifetime = next_context;
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
    cap::Ref te = cap::lookup(*caller.cap_table, thread_cap, CAP_THREAD);
    if (!te) return E_NOTFOUND;
    if ((te->rights_mask & RIGHT_THREAD_CONTROL) == 0) return E_PERM;
    auto* t = static_cast<Thread*>(te->target);
    SysError rc = E_OK;
    auto ctx = sched_context_from_cap(caller, sched_context_cap, &rc);
    if (!ctx) return rc;
    return bind_thread_to_context(t, ctx) ? E_OK : E_BUSY;
}

SysError bind_current_to_sched_context_id(u32 sched_context_id) {
    Thread* t = current();
    auto ctx = sched_context_by_id(sched_context_id);
    if (!t || !ctx) return E_NOTFOUND;
    // Current runner is already running, so bind only fields that affect future
    // accounting and re-enqueue.  Do not migrate it mid-syscall.
    t->principal_id = ctx->principal_id;
    t->sched_context_id = ctx->id;
    auto* next_context = object::acquire(ctx);
    object::release(t->sched_context_lifetime); t->sched_context_lifetime = next_context;
    t->sched_context = ctx;
    t->sched.sched_class = ctx->band;
    t->sched.weight = ctx->weight ? ctx->weight : NICE_0_WEIGHT;
    t->sched.slice_budget_ns = ctx->budget_ns;
    t->affinity_mask = ctx->core_mask;
    t->home_cpu = ctx->home_cpu;
    return E_OK;
}

} // namespace sched

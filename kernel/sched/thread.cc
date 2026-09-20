// kernel/sched/thread.cc — thread and process lifecycle.
//
// Spec ref: §5.3 Address Spaces, §5.4 Threads and Scheduler
//
// V0: one thread per process by default.
// Thread and Process objects are allocated from the kernel heap (mm::heap_alloc).

#include "anko/sched.h"
#include "anko/ipc.h"
#include "anko/vm.h"
#include "anko/mm.h"
#include "anko/panic.h"
#include "anko/smp.h"
#include "anko/spinlock.h"
#include "libk/string.h"
#include "sched_internal.h"

namespace sched {

static u32 g_next_tid = 1;
static u32 g_terminating_threads;
bool termination_pending() { return __atomic_load_n(&g_terminating_threads, __ATOMIC_ACQUIRE) != 0; }
static u32 g_next_pid = 1;
static constexpr u32 kMaxThreads = 128;
static constexpr u32 kMaxProcs = 64;
static constexpr u32 kSandboxPageQuota = 1024;
static Thread* g_threads[kMaxThreads + 1];
static Process* g_processes[kMaxProcs + 1];
static bool g_tid_reserved[kMaxThreads + 1];
static bool g_pid_reserved[kMaxProcs + 1];
static u32 g_pid_generation[kMaxProcs + 1];
static kernel::SpinLock g_lifecycle_lock{};
struct ExitRecord { u64 identity; u64 supervisor; long status; bool ready; };
// A PID remains unavailable while its exit record is unacknowledged. Therefore
// every possible child has a reserved slot and simultaneous deaths cannot drop.
static ExitRecord g_exits[kMaxProcs + 1];
u64 process_identity(const Process& p) { return (static_cast<u64>(p.generation) << 32) | p.pid; }

void record_exit(Process* p, long status) {
    if (!p || !p->supervisor_identity) return;
    kernel::LockGuard guard(g_lifecycle_lock);
    auto& event = g_exits[p->pid];
    if (!event.identity) event = {process_identity(*p), p->supervisor_identity, status, false};
}

bool poll_exit(Process& supervisor, u64* identity, long* status) {
    kernel::LockGuard guard(g_lifecycle_lock);
    for (auto& event : g_exits) {
        if (!event.ready || event.supervisor != process_identity(supervisor)) continue;
        *identity = event.identity; *status = event.status; event = {};
        return true;
    }
    return false;
}

static u32 alloc_pid_locked() {
    for (u32 scan = 0; scan < kMaxProcs; ++scan) {
        const u32 pid = ((g_next_pid - 1 + scan) % kMaxProcs) + 1;
        if (!g_processes[pid] && !g_pid_reserved[pid] && !g_exits[pid].identity) {
            g_pid_reserved[pid] = true;
            g_next_pid = (pid % kMaxProcs) + 1;
            return pid;
        }
    }
    return 0;
}

static u32 alloc_tid_locked() {
    for (u32 scan = 0; scan < kMaxThreads; ++scan) {
        const u32 tid = ((g_next_tid - 1 + scan) % kMaxThreads) + 1;
        if (!g_threads[tid] && !g_tid_reserved[tid]) {
            g_tid_reserved[tid] = true;
            g_next_tid = (tid % kMaxThreads) + 1;
            return tid;
        }
    }
    return 0;
}

static void release_reserved_pid(u32 pid) {
    kernel::LockGuard guard(g_lifecycle_lock);
    if (pid <= kMaxProcs && !g_processes[pid]) g_pid_reserved[pid] = false;
}

static void register_process(Process* p) {
    if (!p) return;
    if (p->pid <= kMaxProcs) {
        g_processes[p->pid] = p;
        g_pid_reserved[p->pid] = false;
    }
}

static void unregister_process(Process* p) {
    if (!p) return;
    if (p->pid <= kMaxProcs) {
        g_processes[p->pid] = nullptr;
        g_pid_reserved[p->pid] = true;
    }
}

static void register_thread(Thread* t) {
    if (!t) return;
    if (t->tid <= kMaxThreads) {
        g_threads[t->tid] = t;
        g_tid_reserved[t->tid] = false;
    }
}

static void unregister_thread(Thread* t) {
    if (!t) return;
    if (t->tid <= kMaxThreads) {
        g_threads[t->tid] = nullptr;
        g_tid_reserved[t->tid] = false;
    }
}

static void destroy_process_storage(void* target) {
    auto* p = static_cast<Process*>(target);
    vm::destroy_owned_process(process_identity(*p));
    VmSpace space{p->ttbr0, p->pid};
    mm::vm_space_destroy(space);
    if (p->arena_table) {
        mm::arena_clear(*p->arena_table);
        mm::slab_free(p->arena_table, sizeof(ArenaTable), alignof(ArenaTable));
    }
    if (p->cap_table) {
        cap::clear(*p->cap_table);
        mm::slab_free(p->cap_table, sizeof(CapTable), alignof(CapTable));
    }
    release_principal(p->principal_id);
    release_reserved_pid(p->pid);
    mm::slab_free(p, sizeof(Process), alignof(Process));
}

static void destroy_thread_storage(void* target) {
    auto* t = static_cast<Thread*>(target);
    auto* owner = t->owner_lifetime;
    object::release(t->sched_context_lifetime);
    mm::slab_free(t, sizeof(Thread), alignof(Thread));
    object::release(owner);
}

Thread* thread_create(u32 pid, VirtAddr entry, VirtAddr sp, ThreadPriority prio,
                      u64 arg0, u32 flags) {
    Thread* t = static_cast<Thread*>(mm::slab_alloc(sizeof(Thread), alignof(Thread)));
    if (!t) return nullptr;
    libk::memset(t, 0, sizeof(*t));

    {
        kernel::LockGuard guard(g_lifecycle_lock);
        t->tid = alloc_tid_locked();
        if (t->tid == 0) {
            mm::slab_free(t, sizeof(Thread), alignof(Thread));
            return nullptr;
        }
    }
    t->pid = pid;
    t->state = (flags & THREAD_SUSPENDED) ? TS_SUSPENDED : TS_RUNNABLE_QUEUED;
    t->cpu_id = 0;
    t->affinity_mask = 0;  // 0 = any CPU
    t->home_cpu  = kInvalidCpu;
    t->last_cpu  = kInvalidCpu;
    t->heap_idx  = 0;
    t->ctx.lr = entry;
    t->ctx.sp = sp;
    t->tf.x[0] = arg0;
    t->tf.elr = entry;
    t->tf.spsr = 0;
    t->tf.sp_el0 = sp;
    t->next = nullptr;
    t->process_next = nullptr;
    t->remote_next = nullptr;
    t->waiting_doorbell = nullptr;
    t->reply_target = nullptr;

    // V2 runtime accounting — all zeroed by memset except the sentinel values above.
    t->runtime_ns           = 0;
    t->vruntime_ns          = 0;   // normalized to min_vruntime on first enqueue
    t->wake_credit_ns       = 0;
    t->last_start_ns        = 0;
    t->last_wake_ns         = 0;
    t->slice_start_ns       = 0;
    t->slice_ns             = 0;
    t->ipc_hot_until_ns     = 0;
    t->drain_quantum_start_ns = 0;
    t->handoff_deadline_ns  = 0;
    t->donated_key          = 0;
    t->in_drain_quantum     = false;
    t->has_donation         = false;
    t->on_runqueue          = false;

    // Scheduling class mapped from legacy priority.
    // Weights follow spec §7.1: IDLE=0, BATCH=512, NORMAL=1024, SERVICE=1536, INTERACTIVE=2048.
    static constexpr SchedClass kPrioClasses[PRIO_COUNT] = {
        SCHED_IDLE, SCHED_BATCH, SCHED_NORMAL, SCHED_SERVICE, SCHED_INTERACTIVE
    };
    static constexpr u32 kPrioWeights[PRIO_COUNT] = {0, 512, 1024, 1536, 2048};
    static_assert(sizeof(kPrioClasses) / sizeof(kPrioClasses[0]) == PRIO_COUNT);
    static_assert(sizeof(kPrioWeights) / sizeof(kPrioWeights[0]) == PRIO_COUNT);
    const u32 prio_idx = (prio < PRIO_COUNT) ? static_cast<u32>(prio) : static_cast<u32>(PRIO_NORMAL);
    t->sched.sched_class        = kPrioClasses[prio_idx];
    t->sched.base_priority      = static_cast<u8>(prio_idx);
    t->sched.effective_priority = static_cast<u8>(prio_idx);
    t->sched.weight             = kPrioWeights[prio_idx];
    t->sched.slice_budget_ns    = 0;

    // Latency hint remains separate from the band. SERVICE and INTERACTIVE both
    // route through SCHED_BAND_LATENCY, but IPC servers get a tighter wake-credit
    // contract than ordinary interactive work.
    switch (prio_idx) {
    case PRIO_SERVICE:  t->latency_hint = LATENCY_IPC_SERVER;  break;
    case PRIO_CRITICAL: t->latency_hint = LATENCY_INTERACTIVE; break;
    case PRIO_BACKGROUND: t->latency_hint = LATENCY_BATCH;     break;
    default:           t->latency_hint = LATENCY_NORMAL;      break;
    }

    auto p = find_process(pid);
    if (!p) {
        kernel::LockGuard guard(g_lifecycle_lock);
        g_tid_reserved[t->tid] = false;
        mm::slab_free(t, sizeof(Thread), alignof(Thread));
        return nullptr;
    }
    t->principal_id = p->principal_id;
    t->owner = p;
    t->owner_lifetime = p->lifetime;
    object::retain(t->owner_lifetime);
    t->lifetime = object::create(t, destroy_thread_storage);
    if (!t->lifetime) {
        object::release(t->owner_lifetime);
        kernel::LockGuard guard(g_lifecycle_lock);
        g_tid_reserved[t->tid] = false;
        mm::slab_free(t, sizeof(Thread), alignof(Thread));
        return nullptr;
    }
    {
        kernel::LockGuard guard(g_lifecycle_lock);
        if (p->dying) {
            g_tid_reserved[t->tid] = false;
            object::release(t->lifetime);
            return nullptr;
        }
        t->process_next = p->threads;
        p->threads = t;
        register_thread(t);
    }
    return t;
}

void thread_destroy(Thread* t) {
    if (!t) return;
    u32 expected = 0;
    if (__atomic_compare_exchange_n(&t->terminate_requested, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __atomic_add_fetch(&g_terminating_threads, 1u, __ATOMIC_RELEASE);
    smp::send_wake_ipi(t->cpu_id);
}

// Scheduler acknowledgement: no CPU queue, pending wake, trap frame or active
// address space may reference this thread when it reaches this function.
void finish_thread(Thread* t) {
    if (!t) return;
    ANKO_ASSERT(t != current() && !t->on_runqueue && !t->remote_next);
    ipc::doorbell_clear_waiter(t);
    handoff_abort_reply(t);
    t->state = TS_DEAD;
    Process* owner = t->owner;
    bool last = false;
    {
        kernel::LockGuard guard(g_lifecycle_lock);
        Thread** link = &owner->threads;
        while (*link && *link != t) link = &(*link)->process_next;
        ANKO_ASSERT(*link == t);
        *link = t->process_next;
        unregister_thread(t);
        last = !owner->threads;
    }
    sweep_process_caps(t);
    if (last) process_destroy(owner);
    __atomic_sub_fetch(&g_terminating_threads, 1u, __ATOMIC_RELEASE);
    object::release(t->lifetime);
}


Process* process_create(PrincipalClass principal, u32 supervisor_pid) {
    Process* p = static_cast<Process*>(mm::slab_alloc(sizeof(Process), alignof(Process)));
    if (!p) return nullptr;
    libk::memset(p, 0, sizeof(*p));

    {
        kernel::LockGuard guard(g_lifecycle_lock);
        p->pid = alloc_pid_locked();
        if (p->pid) {
            if (++g_pid_generation[p->pid] == 0 || g_pid_generation[p->pid] >= 0x80000000) g_pid_generation[p->pid] = 1;
            p->generation = g_pid_generation[p->pid];
        }
        if (p->pid == 0) {
            mm::slab_free(p, sizeof(Process), alignof(Process));
            return nullptr;
        }
    }
    p->principal_class = principal;
    p->supervisor_pid = supervisor_pid;
    if (auto supervisor = find_process(supervisor_pid)) p->supervisor_identity = process_identity(*supervisor);
    if (principal == PRINCIPAL_KERNEL) {
        p->principal_id = 0;
    } else {
        p->principal_id = allocate_principal(principal,
                                             principal == PRINCIPAL_NATIVE_TRUSTED ? 2048 : NICE_0_WEIGHT,
                                             0, 0);
        if (p->principal_id == 0) {
            release_reserved_pid(p->pid);
            mm::slab_free(p, sizeof(Process), alignof(Process));
            return nullptr;
        }
    }
    p->next_stack_slot = 0;
    p->runtime_ns = 0;
    p->page_count = 0;
    p->page_quota = (principal == PRINCIPAL_NATIVE_SANDBOXED) ? kSandboxPageQuota : 0;
    p->runnable_threads = 0;

    VmSpace vm = mm::vm_space_create(p->pid);
    if (!vm.pgd_phys) {
        release_principal(p->principal_id);
        release_reserved_pid(p->pid);
        mm::slab_free(p, sizeof(Process), alignof(Process));
        return nullptr;
    }
    p->ttbr0 = vm.pgd_phys;

    p->cap_table = static_cast<CapTable*>(mm::slab_alloc(sizeof(CapTable), alignof(CapTable)));
    if (!p->cap_table) {
        mm::vm_space_destroy(vm);
        release_principal(p->principal_id);
        release_reserved_pid(p->pid);
        mm::slab_free(p, sizeof(Process), alignof(Process));
        return nullptr;
    }
    libk::memset(p->cap_table, 0, sizeof(CapTable));
    p->cap_table->next_free = 0;

    // Initialize generations to non-zero so handle 0 is never treated as valid.
    for (u32 i = 0; i < kCapTableSize; ++i) {
        p->cap_table->entries[i].generation = 1;
        p->cap_table->entries[i].type_tag = CAP_NULL;
    }

    p->arena_table = static_cast<ArenaTable*>(
        mm::slab_alloc(sizeof(ArenaTable), alignof(ArenaTable)));
    if (!p->arena_table) {
        mm::slab_free(p->cap_table, sizeof(CapTable), alignof(CapTable));
        mm::vm_space_destroy(vm);
        release_principal(p->principal_id);
        release_reserved_pid(p->pid);
        mm::slab_free(p, sizeof(Process), alignof(Process));
        return nullptr;
    }
    libk::memset(p->arena_table, 0, sizeof(ArenaTable));

    p->lifetime = object::create(p, destroy_process_storage);
    if (!p->lifetime) {
        release_reserved_pid(p->pid);
        destroy_process_storage(p);
        return nullptr;
    }
    {
        kernel::LockGuard guard(g_lifecycle_lock);
        register_process(p);
    }
    return p;
}

void process_destroy(Process* p) {
    if (!p) return;
    bool completed = false;
    {
        kernel::LockGuard guard(g_lifecycle_lock);
        if (p->dying == 2) return;
        __atomic_store_n(&p->dying, 1u, __ATOMIC_RELEASE);
        for (Thread* t = p->threads; t; t = t->process_next) thread_destroy(t);
        if (!p->threads) {
            __atomic_store_n(&p->dying, 2u, __ATOMIC_RELEASE);
            unregister_process(p);
            // A dead supervisor cannot acknowledge children. Release their PID
            // reservations instead of stranding them indefinitely.
            for (auto& child : g_exits) if (child.supervisor == process_identity(*p)) child = {};
            completed = true;
        }
    }
    if (completed) {
        sweep_process_caps(p);
        cap::clear(*p->cap_table);
        {
            kernel::LockGuard guard(g_lifecycle_lock);
            auto& event = g_exits[p->pid];
            if (event.identity) event.ready = true;
        }
        for (u32 tid = 1; tid <= kMaxThreads; ++tid) {
            auto waiter = find_thread(tid);
            if (waiter && process_identity(*waiter->owner) == p->supervisor_identity) wake(waiter);
        }
        object::release(p->lifetime);
    }
}

void process_set_name(Process* p, const char* name) {
    if (!p || !name) return;
    usize len = libk::strlen(name);
    if (len >= sizeof(p->name)) len = sizeof(p->name) - 1;
    libk::memcpy(p->name, name, len);
    p->name[len] = '\0';
}

bool process_charge_page(u32 pid) {
    auto p = find_process(pid);
    if (!p) return true;
    u32 count = __atomic_load_n(&p->page_count, __ATOMIC_RELAXED);
    while (true) {
        if (p->page_quota != 0 && count >= p->page_quota) return false;
        const u32 next = count + 1;
        if (__atomic_compare_exchange_n(&p->page_count, &count, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return true;
        }
    }
}

void process_uncharge_page(u32 pid) {
    auto p = find_process(pid);
    if (!p) return;
    u32 count = __atomic_load_n(&p->page_count, __ATOMIC_RELAXED);
    while (count != 0) {
        const u32 next = count - 1;
        if (__atomic_compare_exchange_n(&p->page_count, &count, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

object::Ref<Thread> find_thread(u32 tid) {
    kernel::LockGuard guard(g_lifecycle_lock);
    if (!tid || tid > kMaxThreads) return nullptr;
    return object::Ref<Thread>(g_threads[tid]);
}

object::Ref<Process> find_process(u32 pid) {
    kernel::LockGuard guard(g_lifecycle_lock);
    if (!pid || pid > kMaxProcs) return nullptr;
    return object::Ref<Process>(g_processes[pid]);
}

void sweep_process_caps(void* target) {
    if (!target) return;
    for (u32 i = 1; i <= kMaxProcs; ++i) {
        auto p = find_process(i);
        if (p && p->cap_table) cap::revoke_target(*p->cap_table, target);
    }
}

} // namespace sched

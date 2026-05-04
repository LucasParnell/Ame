// kernel/sched/thread.cc — thread and process lifecycle.
//
// Spec ref: §5.3 Address Spaces, §5.4 Threads and Scheduler
//
// V0: one thread per process by default.
// Thread and Process objects are allocated from the kernel heap (mm::heap_alloc).

#include "anko/sched.h"
#include "anko/ipc.h"
#include "anko/mm.h"
#include "libk/string.h"

namespace sched {

static u32 g_next_tid = 1;
static u32 g_next_pid = 1;
static constexpr u32 kMaxThreads = 128;
static constexpr u32 kMaxProcs = 64;
static Thread* g_threads[kMaxThreads + 1];
static Process* g_processes[kMaxProcs + 1];

static void register_process(Process* p) {
    if (!p) return;
    if (p->pid <= kMaxProcs) g_processes[p->pid] = p;
}

static void unregister_process(Process* p) {
    if (!p) return;
    if (p->pid <= kMaxProcs) g_processes[p->pid] = nullptr;
}

static void register_thread(Thread* t) {
    if (!t) return;
    if (t->tid <= kMaxThreads) g_threads[t->tid] = t;
}

static void unregister_thread(Thread* t) {
    if (!t) return;
    if (t->tid <= kMaxThreads) g_threads[t->tid] = nullptr;
}

Thread* thread_create(u32 pid, VirtAddr entry, VirtAddr sp, ThreadPriority prio,
                      u64 arg0, u32 flags) {
    if (g_next_tid > kMaxThreads) return nullptr;
    Thread* t = static_cast<Thread*>(mm::heap_alloc(sizeof(Thread), alignof(Thread)));
    if (!t) return nullptr;
    libk::memset(t, 0, sizeof(*t));

    t->tid = g_next_tid++;
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
    t->slice_ns             = 0;
    t->ipc_hot_until_ns     = 0;
    t->drain_quantum_start_ns = 0;
    t->handoff_deadline_ns  = 0;
    t->donated_priority     = 0;
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

    // Latency hint from sched_class.
    switch (t->sched.sched_class) {
    case SCHED_INTERACTIVE: t->latency_hint = LATENCY_INTERACTIVE; break;
    case SCHED_SERVICE:     t->latency_hint = LATENCY_IPC_SERVER;  break;
    case SCHED_BATCH:       t->latency_hint = LATENCY_BATCH;       break;
    default:                t->latency_hint = LATENCY_NORMAL;      break;
    }

    Process* p = find_process(pid);
    if (p) {
        t->principal_id = p->principal_id;
        t->process_next = p->threads;
        p->threads = t;
    } else {
        t->principal_id = 0;
    }

    register_thread(t);
    return t;
}

void thread_destroy(Thread* t) {
    if (!t) return;
    // Unlink from owning process's thread list to prevent double-destroy.
    if (Process* p = find_process(t->pid)) {
        Thread** prev = &p->threads;
        while (*prev && *prev != t) prev = &(*prev)->process_next;
        if (*prev) *prev = t->process_next;
    }
    ipc::doorbell_clear_waiter(t);
    dequeue_thread(t);  // remove from fair queue before marking dead
    t->state = TS_DEAD;
    unregister_thread(t);
}

Process* process_create(Principal principal, u32 supervisor_pid) {
    if (g_next_pid > kMaxProcs) return nullptr;
    Process* p = static_cast<Process*>(mm::heap_alloc(sizeof(Process), alignof(Process)));
    if (!p) return nullptr;
    libk::memset(p, 0, sizeof(*p));

    p->pid = g_next_pid++;
    p->principal = principal;
    p->supervisor_pid = supervisor_pid;
    p->principal_id = static_cast<u32>(principal);
    p->next_stack_slot = 0;
    p->runtime_ns = 0;
    p->runnable_threads = 0;

    VmSpace vm = mm::vm_space_create(p->pid);
    p->ttbr0 = vm.pgd_phys;

    p->cap_table = static_cast<CapTable*>(mm::heap_alloc(sizeof(CapTable), alignof(CapTable)));
    if (!p->cap_table) return nullptr;
    libk::memset(p->cap_table, 0, sizeof(CapTable));
    p->cap_table->next_free = 0;

    // Initialize generations to non-zero so handle 0 is never treated as valid.
    for (u32 i = 0; i < kCapTableSize; ++i) {
        p->cap_table->entries[i].generation = 1;
        p->cap_table->entries[i].type_tag = CAP_NULL;
    }

    p->arena_table = static_cast<ArenaTable*>(
        mm::heap_alloc(sizeof(ArenaTable), alignof(ArenaTable)));
    if (!p->arena_table) return nullptr;
    libk::memset(p->arena_table, 0, sizeof(ArenaTable));

    register_process(p);
    return p;
}

void process_destroy(Process* p) {
    if (!p) return;

    Thread* t = p->threads;
    while (t) {
        Thread* next = t->process_next;
        thread_destroy(t);
        t = next;
    }
    p->threads = nullptr;

    VmSpace vm{};
    vm.pgd_phys = p->ttbr0;
    vm.pid = p->pid;
    mm::vm_space_destroy(vm);
    p->ttbr0 = 0;

    unregister_process(p);
}

void process_set_name(Process* p, const char* name) {
    if (!p || !name) return;
    usize len = libk::strlen(name);
    if (len >= sizeof(p->name)) len = sizeof(p->name) - 1;
    libk::memcpy(p->name, name, len);
    p->name[len] = '\0';
}

Thread* find_thread(u32 tid) {
    if (tid == 0 || tid > kMaxThreads) return nullptr;
    return g_threads[tid];
}

Process* find_process(u32 pid) {
    if (pid == 0 || pid > kMaxProcs) return nullptr;
    return g_processes[pid];
}

} // namespace sched

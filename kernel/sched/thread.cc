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
static Thread* g_threads[kMaxThreads];
static Process* g_processes[kMaxProcs];

static void register_process(Process* p) {
    if (!p) return;
    if (p->pid < kMaxProcs) g_processes[p->pid] = p;
}

static void unregister_process(Process* p) {
    if (!p) return;
    if (p->pid < kMaxProcs) g_processes[p->pid] = nullptr;
}

static void register_thread(Thread* t) {
    if (!t) return;
    if (t->tid < kMaxThreads) g_threads[t->tid] = t;
}

static void unregister_thread(Thread* t) {
    if (!t) return;
    if (t->tid < kMaxThreads) g_threads[t->tid] = nullptr;
}

Thread* thread_create(u32 pid, VirtAddr entry, VirtAddr sp, ThreadPriority prio,
                      u64 arg0, u32 flags) {
    Thread* t = static_cast<Thread*>(mm::heap_alloc(sizeof(Thread), alignof(Thread)));
    if (!t) return nullptr;
    libk::memset(t, 0, sizeof(*t));

    t->tid = g_next_tid++;
    t->pid = pid;
    t->state = (flags & THREAD_SUSPENDED) ? TS_SUSPENDED : TS_RUNNABLE;
    t->priority = prio;
    t->cpu_id = 0;
    t->affinity_mask = 1ull;
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

    // Initialize scheduling attributes from the legacy priority level.
    static constexpr u32 kPrioWeights[PRIO_COUNT] = {256, 512, 1024, 2048, 4096};
    static_assert(sizeof(kPrioWeights) / sizeof(kPrioWeights[0]) == PRIO_COUNT,
                  "kPrioWeights must have one entry per ThreadPriority");
    static constexpr SchedClass kPrioClasses[PRIO_COUNT] = {
        SCHED_IDLE, SCHED_BATCH, SCHED_NORMAL, SCHED_SERVICE, SCHED_INTERACTIVE
    };
    static_assert(sizeof(kPrioClasses) / sizeof(kPrioClasses[0]) == PRIO_COUNT,
                  "kPrioClasses must have one entry per ThreadPriority");
    const u32 prio_idx = (prio < PRIO_COUNT) ? static_cast<u32>(prio) : static_cast<u32>(PRIO_NORMAL);
    t->sched.sched_class       = kPrioClasses[prio_idx];
    t->sched.base_priority     = static_cast<u8>(prio_idx);
    t->sched.effective_priority = static_cast<u8>(prio_idx);
    t->sched.weight            = kPrioWeights[prio_idx];
    t->sched.slice_budget_ns   = 0;

    Process* p = find_process(pid);
    if (p) {
        t->process_next = p->threads;
        p->threads = t;
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
    t->state = TS_DEAD;
    unregister_thread(t);
}

Process* process_create(Principal principal, u32 supervisor_pid) {
    Process* p = static_cast<Process*>(mm::heap_alloc(sizeof(Process), alignof(Process)));
    if (!p) return nullptr;
    libk::memset(p, 0, sizeof(*p));

    p->pid = g_next_pid++;
    p->principal = principal;
    p->supervisor_pid = supervisor_pid;
    p->principal_id = static_cast<u32>(principal);
    p->next_stack_slot = 0;
    p->runtime_ticks = 0;
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

Thread* find_thread(u32 tid) {
    if (tid == 0 || tid >= kMaxThreads) return nullptr;
    return g_threads[tid];
}

Process* find_process(u32 pid) {
    if (pid == 0 || pid >= kMaxProcs) return nullptr;
    return g_processes[pid];
}

} // namespace sched

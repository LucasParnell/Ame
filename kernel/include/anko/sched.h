#pragma once
// anko/sched.h — thread, process, and scheduler data layouts.
//
// Spec ref: §5.3 Address Spaces, §5.4 Threads and Scheduler, §5.6 Principals
//
// V2 scheduler: Ressha/Eki.  Ressha is the overall scheduler design; each CPU
// owns one local Eki scheduler station.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/mm.h"
#include "anko/syscall.h"

// ---------------------------------------------------------------------------
// Priority levels (spec §5.4)
// ---------------------------------------------------------------------------
enum ThreadPriority : u8 {
    PRIO_IDLE     = 0,
    PRIO_BACKGROUND = 1,
    PRIO_NORMAL   = 2,
    PRIO_SERVICE  = 3,
    PRIO_CRITICAL = 4,
    PRIO_COUNT    = 5,
};

// ---------------------------------------------------------------------------
// Thread states (spec §5.4)
// ---------------------------------------------------------------------------
enum ThreadState : u8 {
    TS_RUNNABLE = 0,
    TS_RUNNING  = 1,
    TS_BLOCKED  = 2, // waiting on a synchronous operation
    TS_SLEEPING = 3, // waiting on a doorbell
    TS_SUSPENDED = 4,
    TS_DEAD     = 5,
};

// ---------------------------------------------------------------------------
// Wake reasons — why a thread was transitioned out of a blocked state.
// ---------------------------------------------------------------------------
enum WakeReason : u8 {
    WAKE_NONE     = 0,
    WAKE_YIELD    = 1,
    WAKE_TIMER    = 2,
    WAKE_DOORBELL = 3,
    WAKE_NOTIFY   = 4,
    WAKE_REMOTE   = 5,
    WAKE_RESUME   = 6,
    WAKE_IRQ      = 7,
    WAKE_DEBUG    = 8,
};

// ---------------------------------------------------------------------------
// Principal classes (spec §5.6)
// V0 uses these for syscall permission checks and spawn policy.
// ---------------------------------------------------------------------------
enum Principal : u8 {
    PRINCIPAL_KERNEL          = 0,
    PRINCIPAL_NATIVE_TRUSTED  = 1,
    PRINCIPAL_NATIVE_SANDBOXED = 2,
};

// ---------------------------------------------------------------------------
// Scheduling classes and per-thread scheduling attributes.
// ---------------------------------------------------------------------------
enum SchedClass : u8 {
    SCHED_IDLE        = 0,
    SCHED_BATCH       = 1,
    SCHED_NORMAL      = 2,
    SCHED_SERVICE     = 3,
    SCHED_INTERACTIVE = 4,
    SCHED_RT          = 5,
    SCHED_VM          = 6,
};

struct SchedAttrs {
    SchedClass sched_class;
    u8         base_priority;
    u8         effective_priority;
    u8         _pad;
    u32        weight;           // NICE_0_WEIGHT = 1024
    u64        slice_budget_ns;  // 0 = use class default
};
static_assert(__is_trivially_constructible(SchedAttrs));

// ---------------------------------------------------------------------------
// Saved CPU context — what context_switch() saves and restores.
// AArch64: callee-saved registers + SP + PC (as LR at call site).
// ---------------------------------------------------------------------------
struct CpuContext {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // callee-saved
    u64 fp;  // x29
    u64 lr;  // x30 — return address used by context_switch
    u64 sp;
};

// ---------------------------------------------------------------------------
// User trapframe saved/restored on syscall boundaries.
// Matches kernel/arch/arm64/syscall.S exactly.
// ---------------------------------------------------------------------------
struct TrapFrame {
    u64 x[31];   // x0..x30
    u64 elr;     // ELR_EL1
    u64 spsr;    // SPSR_EL1
    u64 sp_el0;  // user stack pointer
};

// ---------------------------------------------------------------------------
// Thread — one schedulable unit.
// V0: one thread per process is the default; more allowed for scheduler tests.
// ---------------------------------------------------------------------------
struct Doorbell;

struct Thread {
    CpuContext   ctx;          // must be first: context_switch() references offset 0
    u32          tid;
    u32          pid;          // owning process
    ThreadState  state;
    ThreadPriority priority;
    WakeReason   last_wake_reason;  // why this thread was last woken
    u32          cpu_id;
    u64          affinity_mask;
    TrapFrame    tf;
    Thread*      next;         // intrusive linked list in run queue
    Thread*      process_next; // intrusive linked list in owning process
    Thread*      remote_next;  // intrusive linked list in pending-wake stack
    Doorbell*    waiting_doorbell;
    u64          switches;
    u64          runtime_ticks;
    SchedAttrs   sched;             // scheduling class and weight
};
static_assert(__is_trivially_constructible(Thread));

// ---------------------------------------------------------------------------
// Process — one isolated address space + cap table.
// Spec §5.3: each process has one address space, one cap table,
// one arena table, a principal class, and a supervisor relationship.
// ---------------------------------------------------------------------------
struct Process {
    u32       pid;
    Principal principal;
    u8        _pad[3];
    u32       supervisor_pid; // process that receives death notifications
    u64       ttbr0;          // physical address of this process's L0 page table
    CapTable* cap_table;      // heap-allocated cap table for this process
    ArenaTable* arena_table;  // heap-allocated per-process arena table
    Thread*   threads;        // linked list of threads
    u32       next_stack_slot;
    u32       principal_id;
    u64       runtime_ticks;
    u32       runnable_threads;
    u32       _pad1;
};

// ---------------------------------------------------------------------------
// Run queue — V0: simple singly-linked list, round-robin.
// One global run queue for single-core V0.
// ---------------------------------------------------------------------------
struct RunQueue {
    Thread* head;
    Thread* tail;
    u32     count;
};

struct PendingWakeQueue {
    Thread* head;
};

// ---------------------------------------------------------------------------
// Per-principal scheduling state (Milestone 2 data model).
// budget_exhausted is tracked but not yet enforced in pick_next.
// ---------------------------------------------------------------------------
struct PrincipalSchedState {
    u32  principal_id;
    u32  weight;
    u64  runtime_ns;
    u64  period_ns;
    u64  budget_ns;
    u64  used_in_period_ns;
    u64  period_start_ns;
    u32  runnable_threads;
    bool budget_exhausted;
    u8   _pad[3];
};

// ---------------------------------------------------------------------------
// Switch reasons (V1 metrics only; V0 policy unchanged).
// ---------------------------------------------------------------------------
enum SwitchReason : u8 {
    SWITCH_NONE  = 0,
    SWITCH_YIELD = 1,
    SWITCH_BLOCK = 2,
    SWITCH_TIMER = 3,
    SWITCH_EXIT  = 4,
    SWITCH_IDLE  = 5,
};

struct Eki {
    u32 cpu_id;
    Thread* current;
    Thread* idle;
    RunQueue queues[PRIO_COUNT];
    PendingWakeQueue pending_wakes;
    bool switch_pending;
    bool requeue_current;
    SwitchReason pending_reason;
    u8 _pad[5];
    u64 context_switches;
    u64 local_wakes;
    u64 remote_wakes;
    u64 ipis_sent;
    u64 ipis_handled;
    u64 pending_wake_drains;
    u64 timer_preempts;
    u64 yield_switches;
    u64 block_switches;
    u64 exit_switches;
    u64 idle_switches;
    u64 wake_doorbell;   // woken by doorbell notify
    u64 wake_remote;     // woken from remote pending-wake queue
    u64 wake_resume;     // woken by resume_thread
};

constexpr u32 kMaxCpus = 4;
constexpr u32 SPAWN_SUSPENDED = 1u << 0;
constexpr u32 THREAD_SUSPENDED = 1u << 0;

namespace sched {

void init();
[[noreturn]] void enter();
[[noreturn]] void enter_secondary(u32 cpu_id);

void tick();
void yield();
void block(Thread* t);
void wake(Thread* t);

Thread* current();
Process* current_process();

Thread* thread_create(u32 pid, VirtAddr entry, VirtAddr sp, ThreadPriority prio,
                      u64 arg0, u32 flags = 0);
void thread_destroy(Thread* t);
Process* process_create(Principal principal, u32 supervisor_pid);
void process_destroy(Process* p);

void enqueue(Thread* t);
bool set_affinity(Thread* t, u64 cpu_mask);
SysError resume_thread(Thread* t);
Thread* find_thread(u32 tid);
Process* find_process(u32 pid);
u32 current_cpu_id();
u32 online_cpu_count();
void set_online_cpu_count(u32 count);
void mark_cpu_online(u32 cpu_id);
void note_ipi_sent(u32 cpu_id);
void note_ipi_handled();
void drain_pending_wakes();

void request_switch(bool requeue_current, SwitchReason reason);
extern "C" void post_syscall(void* frame_ptr);
void mark_current_dead();
u64 context_switch_count();
u64 cpu_context_switch_count(u32 cpu_id);
u64 yield_count();
u64 block_switch_count();
u64 timer_preempt_count();
u64 cpu_timer_preempt_count(u32 cpu_id);
u64 exit_switch_count();
u64 local_wake_count();
u64 remote_wake_count();
u64 ipi_sent_count();
u64 ipi_handled_count();
u64 pending_wake_drain_count();
u64 principal_runtime(u32 principal_id);
u64 fairness_max_lag();
u64 wake_doorbell_count();
u64 wake_remote_count();
u64 wake_resume_count();

} // namespace sched

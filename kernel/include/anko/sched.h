#pragma once
// anko/sched.h — thread, process, and scheduler data layouts.
//
// V2 scheduler: Ressha/Eki fair scheduler with IPC wake credit.
// Each CPU owns one Eki (scheduler station).
// Fairness is hierarchical: principal → thread.
// IPC wakeups get bounded credit; handoff is explicit and same-CPU-first.

#include "anko/types.h"
#include "anko/cap.h"
#include "anko/mm.h"
#include "anko/syscall.h"

// ---------------------------------------------------------------------------
// Legacy priority levels — kept for thread_create API compatibility.
// Internally mapped to SchedClass + weight; not used by the V2 picker.
// ---------------------------------------------------------------------------
enum ThreadPriority : u8 {
    PRIO_IDLE       = 0,
    PRIO_BACKGROUND = 1,
    PRIO_NORMAL     = 2,
    PRIO_SERVICE    = 3,
    PRIO_CRITICAL   = 4,
    PRIO_COUNT      = 5,
};

// ---------------------------------------------------------------------------
// Thread states
// ---------------------------------------------------------------------------
enum ThreadState : u8 {
    TS_RUNNING               = 0, // on a CPU right now
    TS_RUNNABLE_QUEUED       = 1, // in a local fair or RT queue
    TS_RUNNABLE_PENDING_REMOTE = 2, // in a remote pending_wakes stack
    TS_BLOCKED               = 3, // waiting on a synchronous operation
    TS_SLEEPING              = 4, // waiting on a doorbell
    TS_SUSPENDED             = 5, // created suspended; not yet started
    TS_DEAD                  = 6,
};

// ---------------------------------------------------------------------------
// Wake reasons
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
// Principal classes — identity for capability checks and scheduling fairness.
// ---------------------------------------------------------------------------
enum Principal : u8 {
    PRINCIPAL_KERNEL           = 0,
    PRINCIPAL_NATIVE_TRUSTED   = 1,
    PRINCIPAL_NATIVE_SANDBOXED = 2,
};

// ---------------------------------------------------------------------------
// Scheduling classes (V2)
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

// ---------------------------------------------------------------------------
// Latency hints — affect slice, wake credit cap, and IPC drain quantum.
// ---------------------------------------------------------------------------
enum LatencyHint : u8 {
    LATENCY_BATCH       = 0,
    LATENCY_NORMAL      = 1,
    LATENCY_INTERACTIVE = 2,
    LATENCY_IPC_SERVER  = 3,
};

// ---------------------------------------------------------------------------
// IPC scheduling hints — set on Doorbell objects by privileged code.
// ---------------------------------------------------------------------------
enum IpcSchedHint : u32 {
    IPC_HINT_NONE        = 0,
    IPC_HINT_LATENCY     = 1u << 0, // wake receiver with credit
    IPC_HINT_THROUGHPUT  = 1u << 1, // prefer separate CPUs
    IPC_HINT_WAKE_AFFINE = 1u << 2, // colocate receiver with waker
    IPC_HINT_HANDOFF_OK  = 1u << 3, // allow synchronous handoff
    IPC_HINT_SERVER      = 1u << 4, // receiver is a server loop
    IPC_HINT_NO_STEAL    = 1u << 5, // don't migrate this receiver
};

// ---------------------------------------------------------------------------
// Per-thread scheduling attributes.
// ---------------------------------------------------------------------------
struct SchedAttrs {
    SchedClass sched_class;
    u8         base_priority;      // legacy; not used by V2 picker
    u8         effective_priority; // legacy; not used by V2 picker
    u8         _pad;
    u32        weight;             // NICE_0_WEIGHT = 1024
    u64        slice_budget_ns;    // 0 = use class default
};
static_assert(__is_trivially_constructible(SchedAttrs));

// ---------------------------------------------------------------------------
// Saved CPU context for context_switch().  Must be at offset 0 in Thread.
// AArch64 callee-saved registers + SP + PC (as LR at call site).
// ---------------------------------------------------------------------------
struct CpuContext {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // callee-saved
    u64 fp;  // x29
    u64 lr;  // x30
    u64 sp;
};

// ---------------------------------------------------------------------------
// User trapframe saved/restored on syscall entry/exit.
// Matches kernel/arch/arm64/syscall.S exactly.
// ---------------------------------------------------------------------------
struct TrapFrame {
    u64 x[31];   // x0..x30
    u64 elr;     // ELR_EL1
    u64 spsr;    // SPSR_EL1
    u64 sp_el0;  // user SP
};

// ---------------------------------------------------------------------------
// Forward declarations for circular references.
// ---------------------------------------------------------------------------
struct Doorbell;
struct PrincipalEntity;

// ---------------------------------------------------------------------------
// Fair thread queue — min-heap of Thread* keyed by effective_key().
// Capacity is deliberately small: V0 has few threads per principal per CPU.
// ---------------------------------------------------------------------------
constexpr u32 kFairQueueCap = 64;

struct Thread; // forward declaration for FairThreadQueue

struct FairThreadQueue {
    Thread* data[kFairQueueCap];
    u32     size;
};
static_assert(__is_trivially_constructible(FairThreadQueue));

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr u32 kMaxPrincipals = 16;
constexpr u32 kMaxRtPrio     = 16;
constexpr u32 kMaxCpus       = 4;
constexpr u32 kInvalidCpu    = ~0u;
constexpr u32 NICE_0_WEIGHT  = 1024;

// ---------------------------------------------------------------------------
// Per-principal scheduling state — global, one entry per principal_id.
// ---------------------------------------------------------------------------
struct PrincipalSchedState {
    u32  principal_id;
    u32  weight;
    u64  runtime_ns;
    u64  period_ns;
    u64  budget_ns;
    u64  used_in_period_ns;
    u64  period_start_ns;
    u64  vruntime_ns;
    u32  runnable_threads;
    bool throttled;
    u8   _pad[3];
};
static_assert(__is_trivially_constructible(PrincipalSchedState));

// ---------------------------------------------------------------------------
// Per-CPU, per-principal scheduling node.
// One of these exists in each Eki for each principal that has work on that CPU.
// ---------------------------------------------------------------------------
struct PrincipalEntity {
    u32             principal_id;
    u32             weight;
    u64             vruntime_ns;
    u64             runtime_ns;
    u32             runnable_threads;
    bool            throttled;
    bool            in_latency_queue;
    bool            in_timeshare_queue;
    u8              heap_idx;        // position in FairPrincipalQueue heap
    FairThreadQueue threads;         // runnable threads on this CPU
};
static_assert(__is_trivially_constructible(PrincipalEntity));

// ---------------------------------------------------------------------------
// Fair principal queue — min-heap of PrincipalEntity* keyed by vruntime_ns.
// ---------------------------------------------------------------------------
struct FairPrincipalQueue {
    PrincipalEntity* data[kMaxPrincipals];
    u32              size;
};
static_assert(__is_trivially_constructible(FairPrincipalQueue));

// ---------------------------------------------------------------------------
// RT queue — fixed-priority FIFO queues with O(1) bitmap pick.
// ---------------------------------------------------------------------------
struct RunQueue {
    Thread* head;
    Thread* tail;
    u32     count;
};

struct RtQueue {
    RunQueue queues[kMaxRtPrio];
    u32      bitmap;  // bit i set ↔ queues[i] is non-empty
};
static_assert(__is_trivially_constructible(RtQueue));

// ---------------------------------------------------------------------------
// Cross-CPU steal requests.  Each Eki owns one pre-allocated request slot.
// ---------------------------------------------------------------------------
struct StealRequest {
    u32          thief_cpu;
    StealRequest* next;
};

struct PendingStealQueue {
    StealRequest* head; // atomic MPSC stack, same CAS pattern as pending_wakes
};

struct PendingWakeQueue {
    Thread* head;
};

// ---------------------------------------------------------------------------
// Switch reasons (for metrics).
// ---------------------------------------------------------------------------
enum SwitchReason : u8 {
    SWITCH_NONE    = 0,
    SWITCH_YIELD   = 1,
    SWITCH_BLOCK   = 2,
    SWITCH_TIMER   = 3,
    SWITCH_EXIT    = 4,
    SWITCH_IDLE    = 5,
    SWITCH_WAKE    = 6,  // preempted by a higher-priority wakeup
    SWITCH_BUDGET  = 7,  // principal budget exhausted
    SWITCH_REPLY   = 8,  // handoff reply path
    SWITCH_HANDOFF = 9,  // synchronous handoff
};

// ---------------------------------------------------------------------------
// Thread — one schedulable execution context.
//
// ABI note: CpuContext ctx MUST be first (offset 0); assembly references it.
// All new V2 fields are added after the established layout block.
// ---------------------------------------------------------------------------
struct Thread {
    // --- Offset 0: assembly-referenced context block ---
    CpuContext  ctx;              // offset 0, size 104

    // --- Identity ---
    u32         tid;
    u32         pid;
    u32         cpu_id;
    u32         principal_id;    // index into g_principal_sched / Eki::principal_entities
    u32         home_cpu;        // preferred CPU (kInvalidCpu = none)
    u32         last_cpu;        // CPU last ran on

    // --- State flags (byte-packed) ---
    ThreadState state;
    WakeReason  last_wake_reason;
    LatencyHint latency_hint;
    u8          _pad0;

    // --- Fair queue position: heap index within PrincipalEntity.threads ---
    u32         heap_idx;

    // --- Affinity ---
    u64         affinity_mask;   // 0 = any CPU; bit i = CPU i allowed

    // --- Trap frame (user register state) ---
    TrapFrame   tf;

    // --- Intrusive list pointers ---
    Thread*     next;            // link in RT RunQueue
    Thread*     process_next;    // link in Process::threads
    Thread*     remote_next;     // link in PendingWakeQueue stack
    Doorbell*   waiting_doorbell;
    Thread*     reply_target;    // handoff: sender blocked for this receiver's reply

    // --- Runtime accounting ---
    u64         switches;
    u64         runtime_ns;
    u64         vruntime_ns;
    i64         wake_credit_ns;  // bounded IPC dispatch advantage (ns)

    // --- Slice and timing ---
    u64         slice_ns;
    u64         last_start_ns;   // sched_clock_ns() when last switched in
    u64         last_wake_ns;    // sched_clock_ns() when last woken

    // --- IPC state ---
    u64         ipc_hot_until_ns;      // sticky window: don't steal until this time
    u64         drain_quantum_start_ns;
    u64         handoff_deadline_ns;   // when handoff quantum expires
    u64         donated_priority;      // non-transitive effective_key donation from sender

    // --- Boolean flags ---
    bool        in_drain_quantum;
    bool        has_donation;
    bool        on_runqueue;      // physically present in an Eki fair/RT queue
    u8          _pad1[5];

    // --- Scheduling class and weight ---
    SchedAttrs  sched;
};
static_assert(__is_trivially_constructible(Thread));

// ---------------------------------------------------------------------------
// Process — one isolated address space + cap table.
// ---------------------------------------------------------------------------
struct Process {
    u32         pid;
    Principal   principal;
    u8          _pad[3];
    char        name[32];      // human-readable identity (e.g. "init", "io.console")
    u32         supervisor_pid;
    u64         ttbr0;
    CapTable*   cap_table;
    ArenaTable* arena_table;
    Thread*     threads;
    u32         next_stack_slot;
    u32         principal_id;
    u64         runtime_ns;
    u32         runnable_threads;

    // Per-supervisor child-death event slot.  Written by a dying child on
    // one CPU and polled by the supervisor process on another.
    u32         supervisor_event_lock;
    u32         supervisor_event_pending;
    u32         supervisor_event_pid;
    u32         _pad1;
    long        supervisor_event_status;
};

// ---------------------------------------------------------------------------
// Per-CPU scheduler station (Eki).
// ---------------------------------------------------------------------------
struct Eki {
    u32     cpu_id;
    Thread* current;
    Thread* idle;
    Thread* handoff_next;        // non-null: post_syscall uses this instead of pick

    // --- Queues ---
    RtQueue              rt;
    FairPrincipalQueue   latency_principals;   // INTERACTIVE + SERVICE
    FairPrincipalQueue   timeshare_principals; // NORMAL + BATCH + VM
    PrincipalEntity      principal_entities[kMaxPrincipals];

    // --- Cross-CPU coordination ---
    PendingWakeQueue  pending_wakes;
    PendingStealQueue pending_steal_requests;
    StealRequest      steal_req_buf;       // single pre-allocated steal request
    bool              steal_req_in_flight;

    // --- Min-vruntime watermarks (lag detection and normalization) ---
    u64 min_vruntime_latency;
    u64 min_vruntime_timeshare;

    // --- Context switch state ---
    bool         switch_pending;
    bool         requeue_current;
    SwitchReason pending_reason;
    u8           _pad[5];

    // --- Metrics ---
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
    u64 wake_doorbell;
    u64 wake_remote;
    u64 wake_resume;
    u64 steal_requests_sent;
    u64 steal_requests_received;
    u64 steals_granted;
    u64 steals_denied;
    u64 ipc_wake_preemptions;
    u64 handoff_success;
    u64 handoff_fallback;
};
static_assert(__is_trivially_constructible(Eki));

constexpr u32 SPAWN_SUSPENDED  = 1u << 0;
constexpr u32 THREAD_SUSPENDED = 1u << 0;

namespace sched {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void init();
[[noreturn]] void enter();
[[noreturn]] void enter_secondary(u32 cpu_id);

// ---------------------------------------------------------------------------
// Scheduler operations
// ---------------------------------------------------------------------------
void tick();
void yield();
void block(Thread* t);
void wake(Thread* t);

// V2: explicit wake with pre-applied metadata (called after apply_wake_metadata)
void wake_local(Thread* t);
void wake_remote(Thread* t, u32 target_cpu);

// V2: IPC metadata — apply wake credit and record reason before enqueue
void apply_wake_metadata(Thread* t, WakeReason reason, u32 sched_hints);

// V2: wake-affine CPU selection
u32 target_cpu_for_wake(Thread* waiter, Thread* waker,
                        u32 db_sched_hints, u32 db_home_cpu);

// V2: handoff plumbing
void set_handoff_next(Thread* receiver);
bool principal_throttled(u32 principal_id);

// ---------------------------------------------------------------------------
// Thread / process lifecycle
// ---------------------------------------------------------------------------
Thread*  current();
Process* current_process();

Thread*  thread_create(u32 pid, VirtAddr entry, VirtAddr sp, ThreadPriority prio,
                       u64 arg0, u32 flags = 0);
void     thread_destroy(Thread* t);
Process* process_create(Principal principal, u32 supervisor_pid);
void     process_destroy(Process* p);
void     process_set_name(Process* p, const char* name);

void     enqueue(Thread* t);
void     dequeue_thread(Thread* t);
bool     set_affinity(Thread* t, u64 cpu_mask);
SysError resume_thread(Thread* t);
Thread*  find_thread(u32 tid);
Process* find_process(u32 pid);

// ---------------------------------------------------------------------------
// CPU management
// ---------------------------------------------------------------------------
u32  current_cpu_id();
u32  online_cpu_count();
void set_online_cpu_count(u32 count);
void mark_cpu_online(u32 cpu_id);
void note_ipi_sent(u32 cpu_id);
void note_ipi_handled();
void drain_pending_wakes();

void request_switch(bool requeue_current, SwitchReason reason);
extern "C" void post_syscall(void* frame_ptr);
void mark_current_dead();

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------
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

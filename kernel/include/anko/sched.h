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
#include "anko/sched_budget.h"
#include "anko/fpsimd.h"
#include "midix/abi/cpu_limits.h"
struct MxDiagRuntimeBatch;

// ---------------------------------------------------------------------------
// Legacy priority levels — kept for thread_create API compatibility.
// Internally mapped to SchedClass + weight; not used by the V2 picker.
// ---------------------------------------------------------------------------
enum ThreadPriority : u8 {
    PRIO_IDLE = 0,
    PRIO_BACKGROUND = 1,
    PRIO_NORMAL = 2,
    PRIO_SERVICE = 3,
    PRIO_CRITICAL = 4,
    PRIO_COUNT = 5,
};

// ---------------------------------------------------------------------------
// Thread states
// ---------------------------------------------------------------------------
enum ThreadState : u8 {
    TS_RUNNING = 0,                 // on a CPU right now
    TS_RUNNABLE_QUEUED = 1,         // in a local fair or RT queue
    TS_RUNNABLE_PENDING_REMOTE = 2, // in a remote pending_wakes stack
    TS_BLOCKED = 3,                 // waiting on a synchronous operation
    TS_SLEEPING = 4,                // waiting on a doorbell
    TS_SUSPENDED = 5,               // created suspended; not yet started
    TS_DEAD = 6,
};

// State is observed by wakers and teardown on other CPUs. Queue and frame
// ownership belongs to the station while running/queued. Blocking transitions
// and remote publication additionally use the lifecycle lock; publishing a byte
// alone is not permission to dispatch a thread.
struct PublishedThreadState {
    ThreadState value;
    operator ThreadState() const {
        return __atomic_load_n(&value, __ATOMIC_ACQUIRE);
    }
    PublishedThreadState& operator=(ThreadState state) {
        __atomic_store_n(&value, state, __ATOMIC_RELEASE);
        return *this;
    }
};

// ---------------------------------------------------------------------------
// Wake reasons
// ---------------------------------------------------------------------------
enum WakeReason : u8 {
    WAKE_NONE = 0,
    WAKE_YIELD = 1,
    WAKE_TIMER = 2,
    WAKE_DOORBELL = 3,
    WAKE_NOTIFY = 4,
    WAKE_REMOTE = 5,
    WAKE_RESUME = 6,
    WAKE_IRQ = 7,
    WAKE_DEBUG = 8,
};

// ---------------------------------------------------------------------------
// Principal classes — identity for capability checks and authorization.
// Principal IDs are now unique fairness buckets allocated per process/VM.
// ---------------------------------------------------------------------------
enum PrincipalClass : u8 {
    PRINCIPAL_KERNEL = 0,
    PRINCIPAL_NATIVE_TRUSTED = 1,
    PRINCIPAL_NATIVE_SANDBOXED = 2,
};

struct Principal {
    u32 id;
    PrincipalClass class_;
    u32 weight;
    u64 budget_ns;
    u64 period_ns;
};
static_assert(__is_trivially_constructible(Principal));

// ---------------------------------------------------------------------------
// Scheduling bands.  Legacy SchedClass names are kept as aliases so the old
// thread_create/syscall ABI keeps building while policy moves to SchedContext.
// ---------------------------------------------------------------------------
enum SchedBand : u8 {
    SCHED_BAND_RT_NATIVE = 0,
    SCHED_BAND_LATENCY = 1,
    SCHED_BAND_FAIR = 2,
    SCHED_BAND_VM_LATENCY = 3,
    SCHED_BAND_VM_FAIR = 4,
    SCHED_BAND_BATCH = 5,
    SCHED_BAND_IDLE = 6,
};

using SchedClass = SchedBand;
constexpr SchedBand SCHED_INTERACTIVE = SCHED_BAND_LATENCY;
constexpr SchedBand SCHED_SERVICE = SCHED_BAND_LATENCY;
constexpr SchedBand SCHED_NORMAL = SCHED_BAND_FAIR;
constexpr SchedBand SCHED_VM_LATENCY = SCHED_BAND_VM_LATENCY;
constexpr SchedBand SCHED_VM_FAIR = SCHED_BAND_VM_FAIR;
constexpr SchedBand SCHED_VM = SCHED_BAND_VM_FAIR;
constexpr SchedBand SCHED_BATCH = SCHED_BAND_BATCH;
constexpr SchedBand SCHED_IDLE = SCHED_BAND_IDLE;

constexpr u32 NUM_BANDS = 7;

// Kernel context. The syscall accepts the separate, stable MxSchedContext wire
// record; aligning our budget window must never change that userspace layout.
struct SchedContext {
    u32 principal_id;
    u32 owner_thread_or_vcpu;

    SchedBand band;
    u32 weight;

    u64 budget_ns;
    u64 period_ns;
    u64 deadline_ns;
    sched::BudgetWindow budget_window;

    u64 core_mask;
    u32 home_cpu;

    u32 flags;
    u32 id;
};
static_assert(__is_trivially_constructible(SchedContext));

enum SchedContextFlags : u32 {
    SCHED_CTX_NO_STEAL = 1u << 0,
    SCHED_CTX_VM_RUNNER = 1u << 1,
    SCHED_CTX_STEALABLE = 1u << 2,
};

// ---------------------------------------------------------------------------
// Latency hints — affect slice, wake credit cap, and IPC drain quantum.
// ---------------------------------------------------------------------------
enum LatencyHint : u8 {
    LATENCY_BATCH = 0,
    LATENCY_NORMAL = 1,
    LATENCY_INTERACTIVE = 2,
    LATENCY_IPC_SERVER = 3,
};

// ---------------------------------------------------------------------------
// IPC scheduling hints — set on Doorbell objects by privileged code.
// ---------------------------------------------------------------------------
enum IpcSchedHint : u32 {
    IPC_HINT_NONE = 0,
    IPC_HINT_LATENCY = MX_IPC_HINT_LATENCY,
    IPC_HINT_THROUGHPUT = MX_IPC_HINT_THROUGHPUT,
    IPC_HINT_WAKE_AFFINE = MX_IPC_HINT_WAKE_AFFINE,
    IPC_HINT_HANDOFF_OK = MX_IPC_HINT_HANDOFF_OK,
    IPC_HINT_SERVER = MX_IPC_HINT_SERVER,
    IPC_HINT_NO_STEAL = MX_IPC_HINT_NO_STEAL,
};

// ---------------------------------------------------------------------------
// Per-thread scheduling attributes.
// ---------------------------------------------------------------------------
struct SchedAttrs {
    SchedClass sched_class;
    u8 base_priority;      // legacy; not used by V2 picker
    u8 effective_priority; // legacy; not used by V2 picker
    u8 _pad;
    u32 weight;          // NICE_0_WEIGHT = 1024
    u64 slice_budget_ns; // 0 = use class default
};
static_assert(__is_trivially_constructible(SchedAttrs));

// ---------------------------------------------------------------------------
// Saved CPU context for context_switch().  Must be at offset 0 in Thread.
// AArch64 callee-saved registers + SP + PC (as LR at call site).
// ---------------------------------------------------------------------------
struct CpuContext {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28; // callee-saved
    u64 fp;                                               // x29
    u64 lr;                                               // x30
    u64 sp;
};

// ---------------------------------------------------------------------------
// User trapframe saved/restored on syscall entry/exit.
// Matches kernel/arch/arm64/syscall.S exactly.
// ---------------------------------------------------------------------------
struct TrapFrame {
    u64 x[31];  // x0..x30
    u64 elr;    // ELR_EL1
    u64 spsr;   // SPSR_EL1
    u64 sp_el0; // user SP
};
static_assert(sizeof(CpuContext) == 104);
static_assert(__builtin_offsetof(TrapFrame, elr) == 248);
static_assert(__builtin_offsetof(TrapFrame, spsr) == 256);
static_assert(__builtin_offsetof(TrapFrame, sp_el0) == 264);

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
    // Empty/root checks use neighboring bytes, rather than opposite ends of
    // the fixed-capacity array. Remaining heap slots stay cold until needed.
    u32 size;
    Thread* data[kFairQueueCap];
};
static_assert(__is_trivially_constructible(FairThreadQueue));

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr u32 kMaxPrincipals = 64;
// Upper bound for the adaptive cross-core poll budget. The ring helper clamps
// to this before it ever spins, so a bad profile cannot become a busy loop.
constexpr u32 kIpcPollBudgetMax = 4096;
constexpr u32 kMaxRtPrio = 16;
constexpr u32 kMaxCpus = MX_HOST_MAX_CPUS;
constexpr u32 kInvalidCpu = ~0u;
constexpr u32 NICE_0_WEIGHT = 1024;

// ---------------------------------------------------------------------------
// Per-principal scheduling state — global, one entry per principal_id.
// ---------------------------------------------------------------------------
struct alignas(64) PrincipalSchedState {
    // Release-published generation (high word) and weight (low word).
    // Zero means unallocated. Policy is immutable while references exist.
    u64 dispatch;
    u64 budget_ns;
    u64 period_ns;
    PrincipalClass class_;
    sched::BudgetWindow budget_window;
};
static_assert(__is_trivially_constructible(PrincipalSchedState));
static_assert(sizeof(PrincipalSchedState) == 64);

// ---------------------------------------------------------------------------
// Per-CPU, per-principal scheduling node.
// One of these exists in each Eki for each principal that has work on that CPU.
// ---------------------------------------------------------------------------
using BandQueue = FairThreadQueue;

// Selection and local fairness accounting share one cache line per principal.
// Fixed-capacity thread heaps live in a separate CPU-owned array. Keep only the
// two clocks that determine scheduling; runtime totals have global/thread owners.
struct alignas(64) PrincipalEntity {
    u32 principal_id;
    u32 weight;
    u32 generation;
    u64 service_vruntime_ns;    // principal share across all non-RT bands
    u64 thread_min_vruntime_ns; // thread clock; never a principal clock
    // A fair band holds at most kFairQueueCap threads; byte counts retain the
    // full queue capacity while keeping selection metadata in this cache line.
    u8 runnable[NUM_BANDS];
    bool throttled;
    bool in_queue[NUM_BANDS];
    u8 heap_idx[NUM_BANDS]; // position in each FairPrincipalQueue heap
};
static_assert(__is_trivially_constructible(PrincipalEntity));
static_assert(kFairQueueCap <= 255);
static_assert(sizeof(PrincipalEntity) == 64);
static_assert(alignof(PrincipalEntity) == 64);
static_assert(__builtin_offsetof(PrincipalEntity, thread_min_vruntime_ns) < 64);

// ---------------------------------------------------------------------------
// Fair principal queue — min-heap keyed by the principal's service clock.
// ---------------------------------------------------------------------------
struct FairPrincipalQueue {
    u32 size;
    SchedBand band;
    u8 _pad[3];
    PrincipalEntity* data[kMaxPrincipals];
};
static_assert(__is_trivially_constructible(FairPrincipalQueue));

// ---------------------------------------------------------------------------
// RT queue — fixed-priority FIFO queues with O(1) bitmap pick.
// ---------------------------------------------------------------------------
struct RunQueue {
    Thread* head;
    Thread* tail;
    u32 count;
};

struct RtQueue {
    RunQueue queues[kMaxRtPrio];
    u32 bitmap; // bit i set ↔ queues[i] is non-empty
};
static_assert(__is_trivially_constructible(RtQueue));

// ---------------------------------------------------------------------------
// Cross-CPU steal requests.  Each Eki owns one pre-allocated request slot.
// ---------------------------------------------------------------------------
struct StealRequest {
    u32 thief_cpu;
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
    SWITCH_NONE = 0,
    SWITCH_YIELD = 1,
    SWITCH_BLOCK = 2,
    SWITCH_TIMER = 3,
    SWITCH_EXIT = 4,
    SWITCH_IDLE = 5,
    SWITCH_WAKE = 6,    // preempted by a higher-priority wakeup
    SWITCH_BUDGET = 7,  // principal budget exhausted
    SWITCH_REPLY = 8,   // handoff reply path
    SWITCH_HANDOFF = 9, // synchronous handoff
};

// ---------------------------------------------------------------------------
// Thread — one schedulable execution context.
//
// ABI note: CpuContext ctx MUST be first (offset 0); assembly references it.
// Kernel-private metadata follows in cache-line working sets. The context at
// offset zero and the separately passed TrapFrame/FPSIMD layouts are stable.
// ---------------------------------------------------------------------------
struct alignas(64) Thread {
    CpuContext ctx; // assembly-referenced context, offset zero
    // Fill the context's alignment gap with latency-sensitive links. Handoff
    // and RT dispatch no longer fetch a line beyond the cold SIMD register file.
    Thread* reply_target;
    Thread* next;
    Thread* process_next;

    // Exactly one coherence line for cross-CPU wake/dispatch ownership. No
    // unrelated thread can share this line through the slab allocator.
    alignas(64) u32 lifecycle_lock;
    u32 terminate_requested;
    u32 execution_cpu; // release only after the outgoing exception frame is saved
    u32 deferred_wake_cpu;
    u32 tid, pid, cpu_id, principal_id, principal_generation, home_cpu, last_cpu;
    PublishedThreadState state;
    WakeReason last_wake_reason;
    LatencyHint latency_hint;
    // True while this thread's FP/SIMD state is live in the registers. Only a
    // running thread can hold it; switching out saves and clears it. Reuses the
    // former pad byte so the 64-byte layout contract below is unchanged.
    u8 fp_live;
    u32 heap_idx;
    bool in_drain_quantum, has_donation, on_runqueue, migration_rebase;
    u64 affinity_mask;

    // Retained identity and wait publication form a second coherent line.
    object::Record* lifetime;
    object::Record* owner_lifetime;
    object::Record* waiting_lifetime;
    object::Record* sched_context_lifetime;
    struct Process* owner;
    u64 wait_sequence;
    Doorbell* waiting_doorbell;
    Thread* remote_next;

    // CPU-owned scheduling arithmetic; keep it separate from the register file.
    SchedAttrs sched;
    u64 vruntime_ns;
    i64 wake_credit_ns;
    u64 last_start_ns, slice_start_ns, last_wake_ns, slice_ns;

    u64 switches, runtime_ns;
    u64 ipc_hot_until_ns, drain_quantum_start_ns, handoff_deadline_ns, donated_key;
    u32 sched_context_id;
    // Sticky: this thread has executed FP/SIMD at least once. From then on the
    // switch path restores eagerly instead of paying a trap per residency.
    u8 fp_used;
    u8 _pad2[3];
    SchedContext* sched_context;

    alignas(64) TrapFrame tf;
    alignas(64) fpsimd::State fpsimd;
};
static_assert(__is_trivially_constructible(Thread));
static_assert(__builtin_offsetof(Thread, ctx) == 0);
static_assert(alignof(Thread) == 64 && sizeof(Thread) % 64 == 0);
static_assert(__builtin_offsetof(Thread, affinity_mask) + sizeof(u64) -
                  __builtin_offsetof(Thread, lifecycle_lock) ==
              64);
static_assert(__builtin_offsetof(Thread, lifetime) % 64 == 0);
static_assert(__builtin_offsetof(Thread, sched) % 64 == 0);
static_assert(__builtin_offsetof(Thread, tf) % 64 == 0);
static_assert(__builtin_offsetof(Thread, fpsimd) % 64 == 0);

// ---------------------------------------------------------------------------
// Process — isolate dispatch identity, shared accounting, and cold supervision.
// ---------------------------------------------------------------------------
struct alignas(64) Process {
    u32 pid, generation;
    object::Record* lifetime;
    u64 ttbr0;
    CapTable* cap_table;
    ArenaTable* arena_table;
    Thread* threads;
    u32 principal_id, next_stack_slot;
    PrincipalClass principal_class;
    u8 _pad[3];
    u32 dying;

    alignas(64) u64 runtime_ns;
    u32 runnable_threads, page_count, page_quota, lock;

    alignas(64) u64 supervisor_identity;
    u32 supervisor_pid;
    char name[32];
    bool kill_with_supervisor;
    // Monotonic: unrelated service exits must not wait on a running guest's
    // global VM call lock with IRQs masked. Set before publishing the first VM.
    bool created_vm;
};
static_assert(alignof(Process) == 64 && sizeof(Process) == 192);
static_assert(__builtin_offsetof(Process, runtime_ns) == 64);
static_assert(__builtin_offsetof(Process, supervisor_identity) == 128);

// ---------------------------------------------------------------------------
// Per-CPU scheduler station (Eki).
// ---------------------------------------------------------------------------
struct alignas(64) Eki {
    u32 cpu_id;
    Thread* current;
    Thread* idle;
    Thread* handoff_next; // non-null: post_syscall uses this instead of pick
    u32 handoff_streak;   // bounded bypass while other runnable work exists

    // --- Queues ---
    RtQueue rt;
    FairPrincipalQueue latency_principals;
    FairPrincipalQueue vm_latency_principals;
    FairPrincipalQueue fair_principals;
    FairPrincipalQueue vm_fair_principals;
    FairPrincipalQueue batch_principals;
    FairPrincipalQueue idle_principals;

    // --- Cross-CPU coordination ---
    alignas(64) PendingWakeQueue pending_wakes;
    alignas(64) PendingStealQueue pending_steal_requests;
    alignas(64) StealRequest steal_req_buf; // single pre-allocated steal request
    bool steal_req_in_flight;

    // Only this CPU changes queue counts. Peers read the published summaries,
    // never the mutable heap sizes or nodes.
    alignas(64) u32 queued_threads[NUM_BANDS];
    alignas(64) u32 published_load;
    u32 published_steal_load;
    // Peer load readers must not share a line with local dispatch writes.
    alignas(64) u64 min_service_vruntime_ns;

    // --- Context switch state ---
    bool switch_pending;
    bool requeue_current;
    SwitchReason pending_reason;
    // FPEN is open only while the running thread owns the FP/SIMD registers.
    u8 fp_open;
    u8 _pad[7];

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
    u64 fair_queue_overflows;
    u64 handoff_success;
    u64 handoff_fallback;
    // Parallel CPU-owned arrays, indexed by principal ID. Selection and fairness
    // accounting share one-line records; fixed-capacity heap storage is separate.
    PrincipalEntity principal_entities[kMaxPrincipals];
    BandQueue thread_heaps[kMaxPrincipals][NUM_BANDS - 1];
};
static_assert(__is_trivially_constructible(Eki));
static_assert(__builtin_offsetof(Eki, min_service_vruntime_ns) -
                  __builtin_offsetof(Eki, published_load) >=
              64);

constexpr u32 SPAWN_SUSPENDED = 1u << 0;
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
u64 sched_clock_ns();
void tick();
void yield();
void block(Thread* t);
void wake(Thread* t);

// V2: explicit wake with pre-applied metadata (called after apply_wake_metadata)
void wake_local(Thread* t);
void wake_remote(Thread* t, u32 target_cpu);

// V2: IPC metadata — apply wake credit and record reason before enqueue
void apply_wake_metadata(Thread* t, WakeReason reason, u32 sched_hints);
// Doorbell delivery applies credit, selects a CPU, and publishes the wake under
// one lifecycle guard. Returns the selected CPU for the caller's metrics.
u32 wake_ipc(Thread* t, Thread* waker, u32 sched_hints, u32 home_cpu);

// V2: wake-affine CPU selection
u32 target_cpu_for_wake(Thread* waiter, Thread* waker, u32 db_sched_hints, u32 db_home_cpu);

// V2: handoff plumbing
void set_handoff_next(Thread* receiver);
bool principal_throttled(u32 principal_id);
bool switch_pending();
u32 allocate_principal(PrincipalClass class_, u32 weight, u64 budget_ns, u64 period_ns);
void release_principal(u32 principal_id);
CapHandle sched_context_create(Process& caller, const SchedContext& request, SysError* out_error);
SysError sched_context_bind_thread(Process& caller, CapHandle thread_cap,
                                   CapHandle sched_context_cap);
cap::Borrow<SchedContext> sched_context_from_cap(Process& caller, CapHandle sched_context_cap,
                                                 SysError* out_error);
object::Ref<SchedContext> sched_context_by_id(u32 sched_context_id);
SysError bind_current_to_sched_context_id(u32 sched_context_id);

// ---------------------------------------------------------------------------
// Thread / process lifecycle
// ---------------------------------------------------------------------------
Thread* current();
Process* current_process();

Thread* thread_create(u32 pid, VirtAddr entry, VirtAddr sp, ThreadPriority prio, u64 arg0,
                      u32 flags = 0);
void thread_destroy(Thread* t);
Process* process_create(PrincipalClass principal, u32 supervisor_pid);
void process_destroy(Process* p);
void process_set_name(Process* p, const char* name);
bool process_charge_page(u32 pid);
void process_uncharge_page(u32 pid);
void sweep_process_caps(void* target);

// Per-principal IPC wake policy. Budget is the bounded relax count a ring
// helper uses before arming its sleep flag; counters report parks and wakes so
// a userspace controller can retune it without kernel heuristics.
void ipc_note_park(u32 principal_id);
void ipc_note_wake(u32 principal_id);
u32 ipc_poll_budget(u32 principal_id);
u32 ipc_poll_budget_set(u32 principal_id, u32 budget);
u64 ipc_park_count(u32 principal_id);
u64 ipc_wake_count(u32 principal_id);

void enqueue(Thread* t);
void dequeue_thread(Thread* t);
bool set_affinity(Thread* t, u64 cpu_mask);
SysError resume_thread(Thread* t);
object::Ref<Thread> find_thread(u32 tid);
object::Ref<Process> find_process(u32 pid);
bool process_runtime_snapshot(u32 first_pid, MxDiagRuntimeBatch* out);
bool termination_pending();
u64 process_identity(const Process& process);
void record_exit(Process* process, long status);
bool poll_exit(Process& supervisor, u64* identity, long* status);
SysError bind_exit_doorbell(Process& supervisor, Doorbell* doorbell);
void finish_thread(Thread* thread); // called only after its CPU stops using it
void collect_terminations(Eki& eki, Thread* protected_thread = nullptr);

// ---------------------------------------------------------------------------
// CPU management
// ---------------------------------------------------------------------------
u32 current_cpu_id();
u32 online_cpu_count();
u64 online_cpu_mask();
void set_online_cpu_count(u32 count);
void mark_cpu_online(u32 cpu_id);
void note_ipi_sent(u32 cpu_id);
void note_ipi_handled();
void drain_pending_wakes();

void request_switch(bool requeue_current, SwitchReason reason);
extern "C" void post_syscall(void* frame_ptr);
extern "C" TrapFrame* post_syscall_frame(void* frame_ptr);
// EL0 FP/SIMD access trap: restore the current thread's state and open FPEN.
extern "C" void fpsimd_claim();
void mark_current_dead(bool whole_process = false);

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------
u64 context_switch_count();
u64 cpu_context_switch_count(u32 cpu_id);
u64 yield_count();
u64 block_switch_count();
u64 timer_preempt_count();
u64 cpu_timer_preempt_count(u32 cpu_id);
u64 cpu_ipi_handled_count(u32 cpu_id);
u64 exit_switch_count();
u64 local_wake_count();
u64 remote_wake_count();
u64 ipi_sent_count();
u64 ipi_handled_count();
u64 pending_wake_drain_count();
u64 busy_runtime_ns();
u64 principal_runtime(u32 principal_id);
u64 principal_class_runtime(PrincipalClass class_);
u64 fairness_max_lag();
u64 wake_doorbell_count();
u64 wake_remote_count();
u64 wake_resume_count();
u64 wrong_cpu_dequeue_count();

} // namespace sched

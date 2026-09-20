#pragma once
#include "midix/abi/syscall.h"
#include "midix/abi/status.h"
// anko/syscall.h — syscall numbers and ABI.
//
// Spec ref: §6 Syscall ABI
//
// AArch64 calling convention:
//   x0..x5   arguments
//   x8       syscall number
//   svc #0   enter kernel
//   x0       return value
//   x1       secondary return / error detail
//
// Errors are negative integers.

// ---------------------------------------------------------------------------
// Syscall numbers — exact V0 set (spec §6)
// ---------------------------------------------------------------------------
enum Syscall : unsigned {
    // Process / thread
    SYS_YIELD = MX_NR_YIELD,
    SYS_EXIT = MX_NR_EXIT,
    // sys_spawn(image_ptr, image_size, principal, supervisor_cap, flags)
    //   -> x0 process_cap, x1 main_thread_cap
    SYS_SPAWN = MX_NR_SPAWN,
    SYS_KILL = MX_NR_KILL,

    // Memory
    SYS_MAP_PAGE = MX_NR_MAP_PAGE,
    SYS_UNMAP_PAGE = MX_NR_UNMAP_PAGE,
    SYS_ARENA_CREATE = MX_NR_ARENA_CREATE,
    SYS_ARENA_REGISTER = MX_NR_ARENA_REGISTER,

    // Capabilities
    SYS_CAP_DUP = MX_NR_CAP_DUP,
    SYS_CAP_REVOKE = MX_NR_CAP_REVOKE,
    SYS_CAP_IDENTIFY = MX_NR_CAP_IDENTIFY,

    // IPC
    SYS_RING_CREATE = MX_NR_RING_CREATE,
    SYS_RING_SHARE = MX_NR_RING_SHARE,
    SYS_DOORBELL_CREATE = MX_NR_DOORBELL_CREATE,
    SYS_DOORBELL_SHARE = MX_NR_DOORBELL_SHARE,
    SYS_NOTIFY = MX_NR_NOTIFY,
    SYS_WAIT_DOORBELL = MX_NR_WAIT_DOORBELL,

    // Debug (V0 bring-up only — spec §6)
    SYS_DEBUG_PUTS = MX_NR_DEBUG_PUTS,
    SYS_DEBUG_COUNTERS = MX_NR_DEBUG_COUNTERS,

    // V0 support extensions. These keep the bring-up path small while the
    // base architecture is still being made spec-complete.
    SYS_CAP_GRANT = MX_NR_CAP_GRANT,
    SYS_DOORBELL_READ = MX_NR_DOORBELL_READ,
    SYS_SUPERVISOR_POLL = MX_NR_SUPERVISOR_POLL,
    SYS_THREAD_CREATE = MX_NR_THREAD_CREATE,
    SYS_THREAD_SET_AFFINITY = MX_NR_THREAD_SET_AFFINITY,
    SYS_THREAD_RESUME = MX_NR_THREAD_RESUME,
    SYS_THREAD_GET_CPU = MX_NR_THREAD_GET_CPU,
    SYS_THREAD_EXIT = MX_NR_THREAD_EXIT,

    // Dango VM scaffolding (V3). Guest execution is intentionally disabled.
    SYS_VM_CREATE = MX_NR_VM_CREATE,
    SYS_VM_DESTROY = MX_NR_VM_DESTROY,
    SYS_VM_INSPECT = MX_NR_VM_INSPECT,
    SYS_VM_MEMORY_CREATE = MX_NR_VM_MEMORY_CREATE,
    SYS_VM_MEMORY_MAP = MX_NR_VM_MEMORY_MAP,
    SYS_VM_MEMORY_UNMAP = MX_NR_VM_MEMORY_UNMAP,
    SYS_VM_MEMORY_INSPECT = MX_NR_VM_MEMORY_INSPECT,
    SYS_VCPU_CREATE = MX_NR_VCPU_CREATE,
    SYS_VCPU_SET_ENTRY = MX_NR_VCPU_SET_ENTRY,
    SYS_VCPU_INSPECT = MX_NR_VCPU_INSPECT,
    SYS_VCPU_RUN = MX_NR_VCPU_RUN, // (vcpu_cap, VcpuRunBudget* in, VcpuRunResult* out)
    SYS_VM_CHANNEL_CREATE = MX_NR_VM_CHANNEL_CREATE,
    SYS_VM_CHANNEL_ENDPOINT = MX_NR_VM_CHANNEL_ENDPOINT,
    SYS_VM_CHANNEL_INSPECT = MX_NR_VM_CHANNEL_INSPECT,
    SYS_VM_MEMORY_WRITE = MX_NR_VM_MEMORY_WRITE,

    // V2 scheduler: synchronous IPC handoff
    SYS_NOTIFY_HANDOFF = MX_NR_NOTIFY_HANDOFF, // notify_handoff(db_cap, payload_word) → reply_value
    SYS_REPLY_HANDOFF = MX_NR_REPLY_HANDOFF, // reply_handoff(reply_value) → E_OK

    // Scheduler contexts and scheduler-visible vCPU run ABI.
    SYS_SCHED_CONTEXT_CREATE = MX_NR_SCHED_CONTEXT_CREATE,      // (SchedContext* in, size) -> sched_context_cap
    SYS_SCHED_CONTEXT_BIND_THREAD = MX_NR_SCHED_CONTEXT_BIND_THREAD, // (thread_cap, sched_context_cap)
    SYS_VCPU_BIND_SCHED_CONTEXT = MX_NR_VCPU_BIND_SCHED_CONTEXT,   // (vcpu_cap, sched_context_cap)
    SYS_VCPU_SET_CORE_MASK = MX_NR_VCPU_SET_CORE_MASK,        // (vcpu_cap, core_mask, home_cpu)
    SYS_VCPU_KICK = MX_NR_VCPU_KICK,                 // (vcpu_cap, reason)
    SYS_VCPU_INJECT_IRQ = MX_NR_VCPU_INJECT_IRQ,           // (vcpu_cap, irq)
    SYS_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER = MX_NR_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER, // (vcpu_cap)

    // Driver support syscalls
    SYS_MAP_MMIO = MX_NR_MAP_MMIO, // (mmio_cap, va, rights) → E_OK
    SYS_IRQ_BIND_DOORBELL = MX_NR_IRQ_BIND_DOORBELL, // (irq_cap, doorbell_cap, flags) → E_OK
    SYS_IRQ_ACK = MX_NR_IRQ_ACK, // (irq_cap) → E_OK
    SYS_CAP_INFO = MX_NR_CAP_INFO, // (cap, MxCapInfo* out, out_len) → E_OK

    // Phase D: PCI device assignment (spec §6, §7, §8.11/§8.12).
    SYS_VM_ASSIGN_DEVICE = MX_NR_VM_ASSIGN_DEVICE,       // (vm_cap, function_cap, flags) -> cap
    SYS_VM_ASSIGN_MAP_MMIO = MX_NR_VM_ASSIGN_MAP_MMIO,   // (assign, bar_idx, ipa, size, rights, off)
    SYS_VM_ASSIGN_ROUTE_IRQ = MX_NR_VM_ASSIGN_ROUTE_IRQ, // (assign, irq_cap, vcpu_cap, irq, flags)
    SYS_VM_ASSIGN_DESTROY = MX_NR_VM_ASSIGN_DESTROY,     // (assign_cap)

    SYS_VM_ASSIGN_EXPOSE_ECAM = MX_NR_VM_ASSIGN_EXPOSE_ECAM, // (assign, ipa, size)
    SYS_CLOCK_NS = MX_NR_CLOCK_NS,
    SYS_COUNT = MX_NR_VM_ASSIGN_EXPOSE_ECAM + 1,
};

static_assert(static_cast<unsigned>(SYS_YIELD) == static_cast<unsigned>(MX_NR_YIELD), "syscall number drift: YIELD");
static_assert(static_cast<unsigned>(SYS_SPAWN) == static_cast<unsigned>(MX_NR_SPAWN), "syscall number drift: SPAWN");
static_assert(static_cast<unsigned>(SYS_RING_CREATE) == static_cast<unsigned>(MX_NR_RING_CREATE), "syscall number drift: RING_CREATE");
static_assert(static_cast<unsigned>(SYS_DOORBELL_CREATE) == static_cast<unsigned>(MX_NR_DOORBELL_CREATE), "syscall number drift: DOORBELL_CREATE");
static_assert(static_cast<unsigned>(SYS_NOTIFY) == static_cast<unsigned>(MX_NR_NOTIFY), "syscall number drift: NOTIFY");
static_assert(static_cast<unsigned>(SYS_WAIT_DOORBELL) == static_cast<unsigned>(MX_NR_WAIT_DOORBELL), "syscall number drift: WAIT_DOORBELL");
static_assert(static_cast<unsigned>(SYS_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER) == static_cast<unsigned>(MX_NR_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER), "syscall number drift: VCPU_BLOCK");
static_assert(static_cast<unsigned>(SYS_MAP_MMIO)          == static_cast<unsigned>(MX_NR_MAP_MMIO),          "syscall number drift: MAP_MMIO");
static_assert(static_cast<unsigned>(SYS_IRQ_BIND_DOORBELL) == static_cast<unsigned>(MX_NR_IRQ_BIND_DOORBELL), "syscall number drift: IRQ_BIND_DOORBELL");
static_assert(static_cast<unsigned>(SYS_IRQ_ACK)           == static_cast<unsigned>(MX_NR_IRQ_ACK),           "syscall number drift: IRQ_ACK");
static_assert(static_cast<unsigned>(SYS_CAP_INFO)          == static_cast<unsigned>(MX_NR_CAP_INFO),          "syscall number drift: CAP_INFO");
static_assert(static_cast<unsigned>(SYS_VM_ASSIGN_DEVICE)    == static_cast<unsigned>(MX_NR_VM_ASSIGN_DEVICE),    "syscall number drift: VM_ASSIGN_DEVICE");
static_assert(static_cast<unsigned>(SYS_VM_ASSIGN_MAP_MMIO)  == static_cast<unsigned>(MX_NR_VM_ASSIGN_MAP_MMIO),  "syscall number drift: VM_ASSIGN_MAP_MMIO");
static_assert(static_cast<unsigned>(SYS_VM_ASSIGN_ROUTE_IRQ) == static_cast<unsigned>(MX_NR_VM_ASSIGN_ROUTE_IRQ), "syscall number drift: VM_ASSIGN_ROUTE_IRQ");
static_assert(static_cast<unsigned>(SYS_VM_ASSIGN_DESTROY)   == static_cast<unsigned>(MX_NR_VM_ASSIGN_DESTROY),   "syscall number drift: VM_ASSIGN_DESTROY");

// ---------------------------------------------------------------------------
// Error codes — negative, returned in x0
// ---------------------------------------------------------------------------
enum SysError : int {
    E_OK         =  0,
    E_INVALID    = -1,  // bad argument
    E_PERM       = -2,  // capability rights violation
    E_NOMEM      = -3,  // physical memory exhausted
    E_NOTFOUND   = -4,  // cap handle not found / generation mismatch
    E_BADTYPE    = -5,  // cap exists but wrong type
    E_OVERFLOW   = -6,  // table full
    E_DEAD       = -7,  // target process/thread is dead
    E_BUSY       = -8,  // object already has a conflicting waiter/owner
    E_FAULT      = -9,  // invalid userspace memory
    E_NOTIMPL    = -10, // ABI exists but implementation is deliberately absent
    E_BADIMAGE   = -14, // malformed or unsupported native image
    E_BADSIG     = -15, // signed manifest failed trust validation
    E_BADCHECKSUM= -16, // CRC/hash mismatch
    E_POLICY     = -17, // manifest/policy denied operation
};

static_assert(static_cast<int>(E_OK) == static_cast<int>(MX_OK), "status drift: OK");
static_assert(static_cast<int>(E_INVALID) == static_cast<int>(MX_ERR_INVALID), "status drift: INVALID");
static_assert(static_cast<int>(E_PERM) == static_cast<int>(MX_ERR_PERM), "status drift: PERM");
static_assert(static_cast<int>(E_NOTFOUND) == static_cast<int>(MX_ERR_NOT_FOUND), "status drift: NOT_FOUND");
static_assert(static_cast<int>(E_BUSY) == static_cast<int>(MX_ERR_BUSY), "status drift: BUSY");
static_assert(static_cast<int>(E_NOTIMPL) == static_cast<int>(MX_ERR_NOT_IMPLEMENTED), "status drift: NOT_IMPLEMENTED");
static_assert(static_cast<int>(E_BADIMAGE) == static_cast<int>(MX_ERR_BAD_IMAGE), "status drift: BAD_IMAGE");
static_assert(static_cast<int>(E_BADSIG) == static_cast<int>(MX_ERR_BAD_SIGNATURE), "status drift: BAD_SIGNATURE");
static_assert(static_cast<int>(E_BADCHECKSUM) == static_cast<int>(MX_ERR_BAD_CHECKSUM), "status drift: BAD_CHECKSUM");
static_assert(static_cast<int>(E_POLICY) == static_cast<int>(MX_ERR_POLICY), "status drift: POLICY");

// ---------------------------------------------------------------------------
// Kernel-side syscall result — packed into x0/x1
// ---------------------------------------------------------------------------
struct SysResult {
    long value;   // x0: return value or negative error
    long detail;  // x1: secondary information
};

// ---------------------------------------------------------------------------
// Debug counter indices for SYS_DEBUG_COUNTERS (a0 argument)
// ---------------------------------------------------------------------------
enum DebugCounter : unsigned {
    DBG_SYSCALL_COUNT         = 0,
    DBG_IPC_SYSCALL_COUNT     = 1,
    DBG_CONTEXT_SWITCHES      = 2,
    DBG_DEBUG_PUTS            = 3,
    DBG_EL0_FAULTS            = 4,
    DBG_DOORBELL_SLEEPS       = 5,
    DBG_DOORBELL_WAKES        = 6,
    DBG_TIMER_TICKS           = 7,
    DBG_DOORBELL_WAIT_CALLS   = 8,
    DBG_DOORBELL_WAIT_IMMED   = 9,
    DBG_NOTIFY_CALLS          = 10,
    DBG_NOTIFY_NO_WAITER      = 11,
    DBG_NOTIFY_WOKE           = 12,
    DBG_YIELD_COUNT           = 13,
    DBG_BLOCK_SWITCHES        = 14,
    DBG_TIMER_PREEMPTS        = 15,
    DBG_EXIT_SWITCHES         = 16,
    DBG_ONLINE_CPUS           = 17,
    DBG_CPU0_CTX_SWITCHES     = 18,
    DBG_CPU1_CTX_SWITCHES     = 19,
    DBG_CPU0_TIMER_PREEMPTS   = 20,
    DBG_CPU1_TIMER_PREEMPTS   = 21,
    DBG_LOCAL_WAKES           = 22,
    DBG_REMOTE_WAKES          = 23,
    DBG_IPI_SENT              = 24,
    DBG_IPI_HANDLED           = 25,
    DBG_PENDING_WAKE_DRAINS   = 26,
    DBG_DOORBELL_LOCAL_WAKES  = 27,
    DBG_DOORBELL_REMOTE_WAKES = 28,
    DBG_FAIRNESS_LAG          = 29,
    DBG_PRINCIPAL_TRUSTED_RT  = 30,
    DBG_PRINCIPAL_SANDBOX_RT  = 31,
    // Keep scheduler counters out of vm::VM_DEBUG_*'s 32..54 range;
    // SYS_DEBUG_COUNTERS checks that range before this enum's switch.
    DBG_WAKE_DOORBELL         = 64,
    DBG_WAKE_REMOTE           = 65,
    DBG_WAKE_RESUME           = 66,
    // IRQ → doorbell delivery (kernel/drivers/gic/irq_table.cc).
    DBG_IRQ_DELIVERED         = 67,
    DBG_IRQ_BOUND             = 68,
    DBG_IRQ_SELFTEST          = 69,
};

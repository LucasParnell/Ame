#pragma once
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
    SYS_YIELD            = 0,
    SYS_EXIT             = 1,
    // sys_spawn(image_ptr, image_size, principal, supervisor_cap, flags)
    //   -> x0 process_cap, x1 main_thread_cap
    SYS_SPAWN            = 2,
    SYS_KILL             = 3,

    // Memory
    SYS_MAP_PAGE         = 4,
    SYS_UNMAP_PAGE       = 5,
    SYS_ARENA_CREATE     = 6,
    SYS_ARENA_REGISTER   = 7,

    // Capabilities
    SYS_CAP_DUP          = 8,
    SYS_CAP_REVOKE       = 9,
    SYS_CAP_IDENTIFY     = 10,

    // IPC
    SYS_RING_CREATE      = 11,
    SYS_RING_SHARE       = 12,
    SYS_DOORBELL_CREATE  = 13,
    SYS_DOORBELL_SHARE   = 14,
    SYS_NOTIFY           = 15,
    SYS_WAIT_DOORBELL    = 16,

    // Debug (V0 bring-up only — spec §6)
    SYS_DEBUG_PUTS       = 17,
    SYS_DEBUG_COUNTERS   = 18,

    // V0 support extensions. These keep the bring-up path small while the
    // base architecture is still being made spec-complete.
    SYS_CAP_GRANT        = 19,
    SYS_DOORBELL_READ    = 20,
    SYS_SUPERVISOR_POLL  = 21,
    SYS_THREAD_CREATE    = 22,
    SYS_THREAD_SET_AFFINITY = 23,
    SYS_THREAD_RESUME    = 24,
    SYS_THREAD_GET_CPU   = 25,
    SYS_THREAD_EXIT      = 26,

    // Dango VM scaffolding (V3). Guest execution is intentionally disabled.
    SYS_VM_CREATE        = 27,
    SYS_VM_DESTROY       = 28,
    SYS_VM_INSPECT       = 29,
    SYS_VM_MEMORY_CREATE = 30,
    SYS_VM_MEMORY_MAP    = 31,
    SYS_VM_MEMORY_UNMAP  = 32,
    SYS_VM_MEMORY_INSPECT = 33,
    SYS_VCPU_CREATE      = 34,
    SYS_VCPU_SET_ENTRY   = 35,
    SYS_VCPU_INSPECT     = 36,
    SYS_VCPU_RUN         = 37,
    SYS_VM_CHANNEL_CREATE = 38,
    SYS_VM_CHANNEL_ENDPOINT = 39,
    SYS_VM_CHANNEL_INSPECT = 40,
    SYS_VM_MEMORY_WRITE = 41,

    SYS_COUNT,
};

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
};

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
};

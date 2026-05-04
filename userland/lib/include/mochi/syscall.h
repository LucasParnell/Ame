#pragma once
// mochi/syscall.h — userspace SVC wrappers.
//
// Spec ref: §6 Syscall ABI
//
// AArch64: x0-x5=args, x8=nr, svc #0, x0=return, x1=detail.
// All wrappers are inline so the library has zero overhead.
// The compiler will keep the register assignments because we use
// the explicit register variables GCC/Clang support for aarch64.

#include <cstdint>

using CapHandle = uint64_t;
constexpr CapHandle kNullCap = 0;

// The SVC instruction is an ABI boundary, not an ordinary compiler-visible
// function call. Tell the optimizer that a syscall may clobber the normal
// AArch64 caller-saved temporaries; otherwise optimized userspace can keep
// loop indices or pointers live in x9..x17 across `svc` and reuse stale values.
#define MOCHI_SVC_CLOBBERS_0 \
    "x1", "x2", "x3", "x4", "x5", "x6", "x7", \
    "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
    "x30", "cc", "memory"
#define MOCHI_SVC_CLOBBERS_1 \
    "x2", "x3", "x4", "x5", "x6", "x7", \
    "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
    "x30", "cc", "memory"
#define MOCHI_SVC_CLOBBERS_2 \
    "x3", "x4", "x5", "x6", "x7", \
    "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
    "x30", "cc", "memory"
#define MOCHI_SVC_CLOBBERS_3 \
    "x4", "x5", "x6", "x7", \
    "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
    "x30", "cc", "memory"
#define MOCHI_SVC_CLOBBERS_4 \
    "x5", "x6", "x7", \
    "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
    "x30", "cc", "memory"
#define MOCHI_SVC_CLOBBERS_5 \
    "x6", "x7", \
    "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", \
    "x30", "cc", "memory"

// Raw SVC wrapper — 1 argument
static inline int64_t _svc1(uint64_t nr, uint64_t a0) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_0);
    return static_cast<int64_t>(x0);
}

// Raw SVC wrapper — 2 arguments
static inline int64_t _svc2(uint64_t nr, uint64_t a0, uint64_t a1) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_1);
    return static_cast<int64_t>(x0);
}

// Raw SVC wrapper — 3 arguments
static inline int64_t _svc3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_2);
    return static_cast<int64_t>(x0);
}

// Raw SVC wrapper — 4 arguments
static inline int64_t _svc4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_3);
    return static_cast<int64_t>(x0);
}

// Raw SVC wrapper — 5 arguments
static inline int64_t _svc5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_4);
    return static_cast<int64_t>(x0);
}

struct SysPair {
    int64_t value;
    int64_t detail;
};

// Raw SVC wrapper — 6 arguments, pair return
static inline SysPair _svc6_pair(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x5 asm("x5") = a5;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_5);
    return {static_cast<int64_t>(x0), static_cast<int64_t>(x1)};
}

// Raw SVC wrapper — 5 arguments, pair return
static inline SysPair _svc5_pair(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_4);
    return {static_cast<int64_t>(x0), static_cast<int64_t>(x1)};
}

static inline SysPair _svc1_pair(uint64_t nr, uint64_t a0) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = 0;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x8)
                 :
                 : MOCHI_SVC_CLOBBERS_1);
    return {static_cast<int64_t>(x0), static_cast<int64_t>(x1)};
}

// ---------------------------------------------------------------------------
// Syscall number constants (must match anko/syscall.h)
// ---------------------------------------------------------------------------
enum : uint64_t {
    NR_YIELD           = 0,
    NR_EXIT            = 1,
    NR_SPAWN           = 2,
    NR_KILL            = 3,
    NR_MAP_PAGE        = 4,
    NR_UNMAP_PAGE      = 5,
    NR_ARENA_CREATE    = 6,
    NR_ARENA_REGISTER  = 7,
    NR_CAP_DUP         = 8,
    NR_CAP_REVOKE      = 9,
    NR_CAP_IDENTIFY    = 10,
    NR_RING_CREATE     = 11,
    NR_RING_SHARE      = 12,
    NR_DOORBELL_CREATE = 13,
    NR_DOORBELL_SHARE  = 14,
    NR_NOTIFY          = 15,
    NR_WAIT_DOORBELL   = 16,
    NR_DEBUG_PUTS      = 17,
    NR_DEBUG_COUNTERS  = 18,
    NR_CAP_GRANT       = 19,
    NR_DOORBELL_READ   = 20,
    NR_SUPERVISOR_POLL = 21,
    NR_THREAD_CREATE   = 22,
    NR_THREAD_SET_AFFINITY = 23,
    NR_THREAD_RESUME   = 24,
    NR_THREAD_GET_CPU  = 25,
    NR_THREAD_EXIT     = 26,
    NR_VM_CREATE       = 27,
    NR_VM_DESTROY      = 28,
    NR_VM_INSPECT      = 29,
    NR_VM_MEMORY_CREATE = 30,
    NR_VM_MEMORY_MAP   = 31,
    NR_VM_MEMORY_UNMAP = 32,
    NR_VM_MEMORY_INSPECT = 33,
    NR_VCPU_CREATE     = 34,
    NR_VCPU_SET_ENTRY  = 35,
    NR_VCPU_INSPECT    = 36,
    NR_VCPU_RUN        = 37,
    NR_VM_CHANNEL_CREATE = 38,
    NR_VM_CHANNEL_ENDPOINT = 39,
    NR_VM_CHANNEL_INSPECT = 40,
    NR_VM_MEMORY_WRITE = 41,
    NR_NOTIFY_HANDOFF  = 42,
    NR_REPLY_HANDOFF   = 43,
};

constexpr uint32_t SPAWN_SUSPENDED = 1u << 0;
constexpr uint32_t THREAD_SUSPENDED = 1u << 0;

// ---------------------------------------------------------------------------
// Typed wrappers — one per V0 syscall (spec §6)
// ---------------------------------------------------------------------------
static inline void sys_yield()                    { _svc1(NR_YIELD, 0); }
static inline void sys_exit(int32_t code)         { _svc1(NR_EXIT, static_cast<uint64_t>(code)); }
static inline SysPair sys_spawn(const void* image, uint64_t image_size,
                                uint32_t principal, CapHandle supervisor,
                                uint32_t flags = 0, const char* name = nullptr) {
    return _svc6_pair(NR_SPAWN,
                      reinterpret_cast<uint64_t>(image),
                      image_size,
                      principal,
                      supervisor,
                      flags,
                      reinterpret_cast<uint64_t>(name));
}
static inline void sys_kill(CapHandle proc)        { _svc1(NR_KILL, proc); }
static inline int64_t sys_map_page(CapHandle cap, uint64_t va, uint32_t rights) {
    return _svc3(NR_MAP_PAGE, cap, va, rights);
}

static inline int64_t sys_arena_create(uint64_t size, uint32_t flags) {
    return _svc2(NR_ARENA_CREATE, size, flags);
}
static inline int64_t sys_arena_register(uint64_t base, uint64_t size, uint32_t flags) {
    return _svc3(NR_ARENA_REGISTER, base, size, flags);
}

static inline int64_t sys_cap_dup(CapHandle src, uint32_t rights) {
    return _svc2(NR_CAP_DUP, src, rights);
}
static inline void sys_cap_revoke(CapHandle cap)  { _svc1(NR_CAP_REVOKE, cap); }
static inline int64_t sys_cap_identify(CapHandle cap) { return _svc1(NR_CAP_IDENTIFY, cap); }
static inline int64_t sys_cap_grant(CapHandle proc, CapHandle src, uint32_t rights) {
    return _svc3(NR_CAP_GRANT, proc, src, rights);
}

static inline int64_t sys_ring_create(uint32_t order, uint32_t flags) {
    return _svc2(NR_RING_CREATE, order, flags);
}
static inline int64_t sys_ring_share(CapHandle ring, CapHandle proc, uint32_t rights) {
    return _svc3(NR_RING_SHARE, ring, proc, rights);
}
static inline int64_t sys_doorbell_create()       { return _svc1(NR_DOORBELL_CREATE, 0); }
static inline int64_t sys_doorbell_share(CapHandle db, CapHandle proc, uint32_t rights) {
    return _svc3(NR_DOORBELL_SHARE, db, proc, rights);
}

static inline int64_t sys_notify(CapHandle db)      { return _svc1(NR_NOTIFY, db); }
static inline int64_t sys_wait_doorbell(CapHandle db, uint64_t expected) {
    return _svc2(NR_WAIT_DOORBELL, db, expected);
}
static inline uint64_t sys_doorbell_read(CapHandle db) {
    return static_cast<uint64_t>(_svc1(NR_DOORBELL_READ, db));
}
static inline SysPair sys_supervisor_poll(CapHandle supervisor) {
    return _svc1_pair(NR_SUPERVISOR_POLL, supervisor);
}

static inline int64_t sys_thread_create(CapHandle proc, uint64_t entry,
                                        uint64_t arg, uint32_t flags) {
    return _svc4(NR_THREAD_CREATE, proc, entry, arg, flags);
}
static inline int64_t sys_thread_set_affinity(CapHandle thread, uint64_t cpu_mask) {
    return _svc2(NR_THREAD_SET_AFFINITY, thread, cpu_mask);
}
static inline int64_t sys_thread_resume(CapHandle thread) {
    return _svc1(NR_THREAD_RESUME, thread);
}
static inline int64_t sys_thread_get_cpu() {
    return _svc1(NR_THREAD_GET_CPU, 0);
}
static inline void sys_thread_exit(int32_t code) {
    _svc1(NR_THREAD_EXIT, static_cast<uint64_t>(code));
}

static inline void sys_debug_puts(const char* s, uint64_t len) {
    _svc2(NR_DEBUG_PUTS, reinterpret_cast<uint64_t>(s), len);
}
static inline int64_t sys_debug_counter(uint64_t id) {
    return _svc1(NR_DEBUG_COUNTERS, id);
}

static inline int64_t sys_vm_create(CapHandle vm_root, uint64_t flags) {
    return _svc2(NR_VM_CREATE, vm_root, flags);
}
static inline int64_t sys_vm_destroy(CapHandle vm) {
    return _svc1(NR_VM_DESTROY, vm);
}
static inline int64_t sys_vm_inspect(CapHandle vm, void* info, uint64_t info_len) {
    return _svc3(NR_VM_INSPECT, vm, reinterpret_cast<uint64_t>(info), info_len);
}
static inline int64_t sys_vm_memory_create(CapHandle vm, uint64_t size, uint64_t flags) {
    return _svc3(NR_VM_MEMORY_CREATE, vm, size, flags);
}
static inline int64_t sys_vm_memory_map(CapHandle vm, CapHandle memory,
                                        uint64_t guest_pa, uint64_t size,
                                        uint32_t rights) {
    return _svc5(NR_VM_MEMORY_MAP, vm, memory, guest_pa, size, rights);
}
static inline int64_t sys_vm_memory_unmap(CapHandle vm, uint64_t guest_pa, uint64_t size) {
    return _svc3(NR_VM_MEMORY_UNMAP, vm, guest_pa, size);
}
static inline int64_t sys_vm_memory_inspect(CapHandle memory, void* info, uint64_t info_len) {
    return _svc3(NR_VM_MEMORY_INSPECT, memory, reinterpret_cast<uint64_t>(info), info_len);
}
static inline int64_t sys_vm_memory_write(CapHandle memory, uint64_t guest_offset,
                                          const void* src, uint64_t len) {
    return _svc4(NR_VM_MEMORY_WRITE,
                 memory,
                 guest_offset,
                 reinterpret_cast<uint64_t>(src),
                 len);
}
static inline int64_t sys_vcpu_create(CapHandle vm, uint64_t flags) {
    return _svc2(NR_VCPU_CREATE, vm, flags);
}
static inline int64_t sys_vcpu_set_entry(CapHandle vcpu, uint64_t entry_pc,
                                         uint64_t arg0, uint64_t arg1) {
    return _svc4(NR_VCPU_SET_ENTRY, vcpu, entry_pc, arg0, arg1);
}
static inline int64_t sys_vcpu_inspect(CapHandle vcpu, void* info, uint64_t info_len) {
    return _svc3(NR_VCPU_INSPECT, vcpu, reinterpret_cast<uint64_t>(info), info_len);
}
static inline int64_t sys_vcpu_run(CapHandle vcpu, uint64_t budget_ticks) {
    return _svc2(NR_VCPU_RUN, vcpu, budget_ticks);
}
static inline int64_t sys_vm_channel_create(CapHandle vm, uint32_t order, uint32_t flags) {
    return _svc3(NR_VM_CHANNEL_CREATE, vm, order, flags);
}
static inline int64_t sys_vm_channel_endpoint(CapHandle channel, uint32_t endpoint_kind,
                                              uint32_t rights) {
    return _svc3(NR_VM_CHANNEL_ENDPOINT, channel, endpoint_kind, rights);
}
static inline int64_t sys_vm_channel_inspect(CapHandle channel, void* info, uint64_t info_len) {
    return _svc3(NR_VM_CHANNEL_INSPECT, channel, reinterpret_cast<uint64_t>(info), info_len);
}

// sys_notify_handoff(db, payload) — synchronous handoff to doorbell waiter.
// Sender blocks until receiver calls sys_reply_handoff; returns reply value.
// Falls back to a regular notify if the receiver is not eligible for handoff.
static inline int64_t sys_notify_handoff(CapHandle db, uint64_t payload) {
    return _svc2(NR_NOTIFY_HANDOFF, db, payload);
}

// sys_reply_handoff(reply_val) — unblock the handoff sender with reply_val.
// Must be called by the thread that received via notify_handoff.
static inline int64_t sys_reply_handoff(uint64_t reply_val) {
    return _svc1(NR_REPLY_HANDOFF, reply_val);
}

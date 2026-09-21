#pragma once
// mochi/syscall.h — userspace SVC wrappers and the safe mx_* ABI layer.
//
// Raw sys_* wrappers preserve the V0 bring-up API. New service code should use
// mx_* wrappers where handles are returned through out-parameters and status is
// explicit. No mx_* helper blocks the kernel except the deliberately named
// mx_doorbell_wait / mx_ring_wait_* APIs in mochi/ring.h.

#include <cstdint>
#include "midix/abi/types.h"
#include "midix/abi/status.h"
#include "midix/abi/cap.h"
#include "midix/abi/syscall.h"

using CapHandle = mx_handle_t;
constexpr CapHandle kNullCap = MX_NULL_HANDLE;

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

static inline int64_t _svc1(uint64_t nr, uint64_t a0) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x8) : : MOCHI_SVC_CLOBBERS_0);
    return static_cast<int64_t>(x0);
}

static inline int64_t _svc2(uint64_t nr, uint64_t a0, uint64_t a1) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x8) : : MOCHI_SVC_CLOBBERS_1);
    return static_cast<int64_t>(x0);
}

static inline int64_t _svc3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x8) : : MOCHI_SVC_CLOBBERS_2);
    return static_cast<int64_t>(x0);
}

static inline int64_t _svc4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x8) : : MOCHI_SVC_CLOBBERS_3);
    return static_cast<int64_t>(x0);
}

static inline int64_t _svc5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x8) : : MOCHI_SVC_CLOBBERS_4);
    return static_cast<int64_t>(x0);
}

static inline int64_t _svc6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x5 asm("x5") = a5;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8) : : MOCHI_SVC_CLOBBERS_5);
    return static_cast<int64_t>(x0);
}

struct SysPair { int64_t value; int64_t detail; };

static inline SysPair _svc6_pair(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x5 asm("x5") = a5;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8) : : MOCHI_SVC_CLOBBERS_5);
    return {static_cast<int64_t>(x0), static_cast<int64_t>(x1)};
}

static inline SysPair _svc5_pair(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x8) : : MOCHI_SVC_CLOBBERS_4);
    return {static_cast<int64_t>(x0), static_cast<int64_t>(x1)};
}

static inline SysPair _svc1_pair(uint64_t nr, uint64_t a0) {
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = 0;
    register uint64_t x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x8) : : MOCHI_SVC_CLOBBERS_1);
    return {static_cast<int64_t>(x0), static_cast<int64_t>(x1)};
}

// Raw sys_* wrappers ---------------------------------------------------------
static inline void sys_yield() { _svc1(NR_YIELD, 0); }
static inline void sys_exit(int32_t code) { _svc1(NR_EXIT, static_cast<uint64_t>(code)); }
static inline SysPair sys_spawn(const void* image, uint64_t image_size,
                                uint32_t principal, CapHandle supervisor,
                                uint32_t flags = 0, const char* name = nullptr) {
    return _svc6_pair(NR_SPAWN, reinterpret_cast<uint64_t>(image), image_size,
                      principal, supervisor, flags, reinterpret_cast<uint64_t>(name));
}
static inline int64_t sys_kill(CapHandle proc) { return _svc1(NR_KILL, proc); }
static inline int64_t sys_map_page(CapHandle cap, uint64_t va, uint32_t rights) { return _svc3(NR_MAP_PAGE, cap, va, rights); }
static inline int64_t sys_unmap_page(uint64_t va) { return _svc1(NR_UNMAP_PAGE, va); }
static inline int64_t sys_arena_create(uint64_t size, uint32_t flags) { return _svc2(NR_ARENA_CREATE, size, flags); }
static inline int64_t sys_arena_register(uint64_t base, uint64_t size, uint32_t flags) { return _svc3(NR_ARENA_REGISTER, base, size, flags); }
static inline int64_t sys_cap_dup(CapHandle src, uint32_t rights) { return _svc2(NR_CAP_DUP, src, rights); }
static inline int64_t sys_cap_revoke(CapHandle cap) { return _svc1(NR_CAP_REVOKE, cap); }
static inline int64_t sys_cap_identify(CapHandle cap) { return _svc1(NR_CAP_IDENTIFY, cap); }
static inline int64_t sys_cap_grant(CapHandle proc, CapHandle src, uint32_t rights) { return _svc3(NR_CAP_GRANT, proc, src, rights); }
static inline int64_t sys_ring_create(uint32_t order, uint32_t flags) { return _svc2(NR_RING_CREATE, order, flags); }
static inline int64_t sys_ring_share(CapHandle ring, CapHandle proc, uint32_t rights) { return _svc3(NR_RING_SHARE, ring, proc, rights); }
static inline int64_t sys_doorbell_create() { return _svc1(NR_DOORBELL_CREATE, 0); }
static inline int64_t sys_doorbell_share(CapHandle db, CapHandle proc, uint32_t rights) { return _svc3(NR_DOORBELL_SHARE, db, proc, rights); }
static inline int64_t sys_notify(CapHandle db) { return _svc1(NR_NOTIFY, db); }
static inline int64_t sys_wait_doorbell(CapHandle db, uint64_t expected) { return _svc2(NR_WAIT_DOORBELL, db, expected); }
static inline uint64_t sys_doorbell_read(CapHandle db) { return static_cast<uint64_t>(_svc1(NR_DOORBELL_READ, db)); }
static inline SysPair sys_supervisor_poll(CapHandle supervisor) { return _svc1_pair(NR_SUPERVISOR_POLL, supervisor); }
static inline int64_t sys_thread_create(CapHandle proc, uint64_t entry, uint64_t arg, uint32_t flags) { return _svc4(NR_THREAD_CREATE, proc, entry, arg, flags); }
static inline int64_t sys_thread_set_affinity(CapHandle thread, uint64_t cpu_mask) { return _svc2(NR_THREAD_SET_AFFINITY, thread, cpu_mask); }
static inline int64_t sys_thread_resume(CapHandle thread) { return _svc1(NR_THREAD_RESUME, thread); }
static inline int64_t sys_thread_get_cpu() { return _svc1(NR_THREAD_GET_CPU, 0); }
static inline void sys_thread_exit(int32_t code) { _svc1(NR_THREAD_EXIT, static_cast<uint64_t>(code)); }
static inline void sys_debug_puts(const char* s, uint64_t len) { _svc2(NR_DEBUG_PUTS, reinterpret_cast<uint64_t>(s), len); }
static inline int64_t sys_debug_counter(uint64_t id) { return _svc1(NR_DEBUG_COUNTERS, id); }
static inline int64_t sys_vm_assign_expose_ecam(CapHandle assignment, uint64_t ipa, uint64_t size) {
    return _svc3(NR_VM_ASSIGN_EXPOSE_ECAM, assignment, ipa, size);
}
static inline int64_t sys_vm_assign_device(CapHandle vm, CapHandle function, uint64_t flags) {
    return _svc3(NR_VM_ASSIGN_DEVICE, vm, function, flags);
}
static inline int64_t sys_vm_assign_map_mmio(CapHandle assignment, uint32_t bar_index, uint64_t guest_ipa,
                                             uint64_t size, uint32_t rights, uint32_t bar_offset) {
    return _svc6(NR_VM_ASSIGN_MAP_MMIO, assignment, bar_index, guest_ipa, size, rights, bar_offset);
}
static inline int64_t sys_vm_assign_route_irq(CapHandle assignment, CapHandle irq, CapHandle vcpu,
                                              uint32_t guest_irq, uint32_t flags) {
    return _svc5(NR_VM_ASSIGN_ROUTE_IRQ, assignment, irq, vcpu, guest_irq, flags);
}
static inline int64_t sys_vm_assign_destroy(CapHandle assignment) {
    return _svc1(NR_VM_ASSIGN_DESTROY, assignment);
}
static inline int64_t sys_vm_create(CapHandle vm_root, uint64_t flags) { return _svc2(NR_VM_CREATE, vm_root, flags); }
static inline int64_t sys_vm_destroy(CapHandle vm) { return _svc1(NR_VM_DESTROY, vm); }
static inline int64_t sys_vm_inspect(CapHandle vm, void* info, uint64_t info_len) { return _svc3(NR_VM_INSPECT, vm, reinterpret_cast<uint64_t>(info), info_len); }
static inline int64_t sys_vm_memory_create(CapHandle vm, uint64_t size, uint64_t flags) { return _svc3(NR_VM_MEMORY_CREATE, vm, size, flags); }
static inline int64_t sys_vm_memory_map(CapHandle vm, CapHandle memory, uint64_t guest_pa, uint64_t size, uint32_t rights) { return _svc5(NR_VM_MEMORY_MAP, vm, memory, guest_pa, size, rights); }
static inline int64_t sys_vm_memory_unmap(CapHandle vm, uint64_t guest_pa, uint64_t size) { return _svc3(NR_VM_MEMORY_UNMAP, vm, guest_pa, size); }
static inline int64_t sys_vm_memory_inspect(CapHandle memory, void* info, uint64_t info_len) { return _svc3(NR_VM_MEMORY_INSPECT, memory, reinterpret_cast<uint64_t>(info), info_len); }
static inline int64_t sys_vm_memory_write(CapHandle memory, uint64_t guest_offset, const void* src, uint64_t len) { return _svc4(NR_VM_MEMORY_WRITE, memory, guest_offset, reinterpret_cast<uint64_t>(src), len); }
static inline int64_t sys_vcpu_create(CapHandle vm, uint64_t flags) { return _svc2(NR_VCPU_CREATE, vm, flags); }
static inline int64_t sys_vcpu_set_entry(CapHandle vcpu, uint64_t entry_pc, uint64_t arg0, uint64_t arg1) { return _svc4(NR_VCPU_SET_ENTRY, vcpu, entry_pc, arg0, arg1); }
static inline int64_t sys_vcpu_inspect(CapHandle vcpu, void* info, uint64_t info_len) { return _svc3(NR_VCPU_INSPECT, vcpu, reinterpret_cast<uint64_t>(info), info_len); }
static inline int64_t sys_vcpu_run(CapHandle vcpu, const void* budget, void* result) { return _svc3(NR_VCPU_RUN, vcpu, reinterpret_cast<uint64_t>(budget), reinterpret_cast<uint64_t>(result)); }
static inline int64_t sys_sched_context_create(const void* ctx, uint64_t ctx_len) { return _svc2(NR_SCHED_CONTEXT_CREATE, reinterpret_cast<uint64_t>(ctx), ctx_len); }
static inline int64_t sys_sched_context_bind_thread(CapHandle thread, CapHandle sched_context) { return _svc2(NR_SCHED_CONTEXT_BIND_THREAD, thread, sched_context); }
static inline int64_t sys_vcpu_bind_sched_context(CapHandle vcpu, CapHandle sched_context) { return _svc2(NR_VCPU_BIND_SCHED_CONTEXT, vcpu, sched_context); }
static inline int64_t sys_vcpu_set_core_mask(CapHandle vcpu, uint64_t core_mask, uint32_t home_cpu) { return _svc3(NR_VCPU_SET_CORE_MASK, vcpu, core_mask, home_cpu); }
static inline int64_t sys_vcpu_kick(CapHandle vcpu, uint32_t reason) { return _svc2(NR_VCPU_KICK, vcpu, reason); }
static inline int64_t sys_vcpu_inject_irq(CapHandle vcpu, uint32_t irq) { return _svc2(NR_VCPU_INJECT_IRQ, vcpu, irq); }
static inline int64_t sys_vcpu_block_until_irq_or_timer(CapHandle vcpu) { return _svc1(NR_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER, vcpu); }
static inline int64_t sys_vm_channel_create(CapHandle vm, uint32_t order, uint32_t flags) { return _svc3(NR_VM_CHANNEL_CREATE, vm, order, flags); }
static inline int64_t sys_vm_channel_endpoint(CapHandle channel, uint32_t endpoint_kind, uint32_t rights) { return _svc3(NR_VM_CHANNEL_ENDPOINT, channel, endpoint_kind, rights); }
static inline int64_t sys_vm_channel_inspect(CapHandle channel, void* info, uint64_t info_len) { return _svc3(NR_VM_CHANNEL_INSPECT, channel, reinterpret_cast<uint64_t>(info), info_len); }
static inline int64_t sys_notify_handoff(CapHandle db, uint64_t payload) { return _svc2(NR_NOTIFY_HANDOFF, db, payload); }
static inline int64_t sys_reply_handoff(uint64_t reply_val) { return _svc1(NR_REPLY_HANDOFF, reply_val); }
static inline int64_t sys_map_mmio(CapHandle mmio_cap, uint64_t va, uint32_t rights) { return _svc3(NR_MAP_MMIO, mmio_cap, va, rights); }
static inline int64_t sys_irq_bind_doorbell(CapHandle irq_cap, CapHandle doorbell_cap, uint32_t flags) { return _svc3(NR_IRQ_BIND_DOORBELL, irq_cap, doorbell_cap, flags); }
static inline int64_t sys_irq_ack(CapHandle irq_cap) { return _svc1(NR_IRQ_ACK, irq_cap); }
static inline int64_t sys_cap_info(CapHandle cap, void* out, uint64_t out_len) { return _svc3(NR_CAP_INFO, cap, reinterpret_cast<uint64_t>(out), out_len); }

// mx_* wrappers (driver support) ---------------------------------------------
static inline mx_status_t mx_map_mmio(mx_handle_t mmio_cap, mx_vaddr_t va, mx_rights_t rights) {
    return sys_map_mmio(mmio_cap, va, rights);
}
static inline mx_status_t mx_irq_bind_doorbell(mx_handle_t irq_cap, mx_handle_t doorbell_cap, uint32_t flags) {
    return sys_irq_bind_doorbell(irq_cap, doorbell_cap, flags);
}
static inline mx_status_t mx_irq_ack(mx_handle_t irq_cap) {
    return sys_irq_ack(irq_cap);
}
static inline mx_status_t mx_cap_info(mx_handle_t cap, MxCapInfo* out) {
    return sys_cap_info(cap, out, sizeof(MxCapInfo));
}

// mx_* wrappers --------------------------------------------------------------
static inline mx_status_t mx_status_from_value(int64_t value, mx_handle_t* out = nullptr) {
    if (value < 0) return value;
    if (out) *out = static_cast<mx_handle_t>(value);
    return MX_OK;
}

static inline uint64_t mx_clock_ns() { return static_cast<uint64_t>(_svc1(MX_NR_CLOCK_NS, 0)); }

static inline void mx_yield() { sys_yield(); }
static inline void mx_exit(int32_t code) { sys_exit(code); }
static inline mx_status_t mx_map_page(mx_handle_t cap, mx_vaddr_t va, mx_rights_t rights) { return sys_map_page(cap, va, rights); }
static inline mx_status_t mx_arena_create(uint64_t size, uint32_t flags, mx_handle_t* out) { return mx_status_from_value(sys_arena_create(size, flags), out); }
static inline mx_status_t mx_cap_identify(mx_handle_t cap, mx_i64_t* out_type) {
    const int64_t rc = sys_cap_identify(cap);
    if (rc < 0) return rc;
    if (out_type) *out_type = rc;
    return MX_OK;
}
static inline mx_status_t mx_cap_grant(mx_handle_t process, mx_handle_t src, mx_rights_t rights, mx_handle_t* out) { return mx_status_from_value(sys_cap_grant(process, src, rights), out); }
static inline mx_status_t mx_cap_dup(mx_handle_t src, mx_rights_t rights, mx_handle_t* out) { return mx_status_from_value(sys_cap_dup(src, rights), out); }
static inline mx_status_t mx_ring_create(uint32_t order, uint32_t flags, mx_handle_t* out) { return mx_status_from_value(sys_ring_create(order, flags), out); }
static inline mx_status_t mx_ring_share(mx_handle_t ring, mx_handle_t process, mx_rights_t rights, mx_handle_t* out) { return mx_status_from_value(sys_ring_share(ring, process, rights), out); }
static inline mx_status_t mx_doorbell_create(mx_handle_t* out) { return mx_status_from_value(sys_doorbell_create(), out); }
static inline mx_status_t mx_doorbell_share(mx_handle_t db, mx_handle_t process, mx_rights_t rights, mx_handle_t* out) { return mx_status_from_value(sys_doorbell_share(db, process, rights), out); }
static inline mx_status_t mx_doorbell_notify(mx_handle_t db) { return sys_notify(db); }
static inline mx_status_t mx_doorbell_wait(mx_handle_t db, uint64_t expected) { return sys_wait_doorbell(db, expected); }
static inline uint64_t mx_doorbell_read(mx_handle_t db) { return sys_doorbell_read(db); }
static inline mx_status_t mx_thread_set_affinity(mx_handle_t thread, uint64_t cpu_mask) { return sys_thread_set_affinity(thread, cpu_mask); }
static inline mx_status_t mx_thread_resume(mx_handle_t thread) { return sys_thread_resume(thread); }
static inline mx_status_t mx_spawn(const void* image, uint64_t image_size, uint32_t principal, mx_handle_t supervisor, uint32_t flags, const char* name, mx_handle_t* out_proc, mx_handle_t* out_thread) {
    const SysPair p = sys_spawn(image, image_size, principal, supervisor, flags, name);
    if (p.value < 0) return p.value;
    if (out_proc) *out_proc = static_cast<mx_handle_t>(p.value);
    if (out_thread) *out_thread = static_cast<mx_handle_t>(p.detail);
    return MX_OK;
}

static inline int64_t sys_vm_channel_map(CapHandle channel, CapHandle vcpu, uint64_t ipa, uint64_t size, uint32_t irq) { return _svc5(NR_VM_CHANNEL_MAP, channel, vcpu, ipa, size, irq); }
static inline int64_t sys_vm_channel_notify(CapHandle channel) { return _svc1(NR_VM_CHANNEL_NOTIFY, channel); }

#pragma once
// Midix syscall numbers and small flag constants. This is the single userspace source of truth.

#include "midix/abi/types.h"

enum MxSyscall : mx_u64_t {
    MX_NR_YIELD           = 0,
    MX_NR_EXIT            = 1,
    MX_NR_SPAWN           = 2,
    MX_NR_KILL            = 3,
    MX_NR_MAP_PAGE        = 4,
    MX_NR_UNMAP_PAGE      = 5,
    MX_NR_ARENA_CREATE    = 6,
    MX_NR_ARENA_REGISTER  = 7,
    MX_NR_CAP_DUP         = 8,
    MX_NR_CAP_REVOKE      = 9,
    MX_NR_CAP_IDENTIFY    = 10,
    MX_NR_RING_CREATE     = 11,
    MX_NR_RING_SHARE      = 12,
    MX_NR_DOORBELL_CREATE = 13,
    MX_NR_DOORBELL_SHARE  = 14,
    MX_NR_NOTIFY          = 15,
    MX_NR_WAIT_DOORBELL   = 16,
    MX_NR_DEBUG_PUTS      = 17,
    MX_NR_DEBUG_COUNTERS  = 18,
    MX_NR_CAP_GRANT       = 19,
    MX_NR_DOORBELL_READ   = 20,
    MX_NR_SUPERVISOR_POLL = 21,
    MX_NR_THREAD_CREATE   = 22,
    MX_NR_THREAD_SET_AFFINITY = 23,
    MX_NR_THREAD_RESUME   = 24,
    MX_NR_THREAD_GET_CPU  = 25,
    MX_NR_THREAD_EXIT     = 26,
    MX_NR_VM_CREATE       = 27,
    MX_NR_VM_DESTROY      = 28,
    MX_NR_VM_INSPECT      = 29,
    MX_NR_VM_MEMORY_CREATE = 30,
    MX_NR_VM_MEMORY_MAP   = 31,
    MX_NR_VM_MEMORY_UNMAP = 32,
    MX_NR_VM_MEMORY_INSPECT = 33,
    MX_NR_VCPU_CREATE     = 34,
    MX_NR_VCPU_SET_ENTRY  = 35,
    MX_NR_VCPU_INSPECT    = 36,
    MX_NR_VCPU_RUN        = 37,
    MX_NR_VM_CHANNEL_CREATE = 38,
    MX_NR_VM_CHANNEL_ENDPOINT = 39,
    MX_NR_VM_CHANNEL_INSPECT = 40,
    MX_NR_VM_MEMORY_WRITE = 41,
    MX_NR_NOTIFY_HANDOFF  = 42,
    MX_NR_REPLY_HANDOFF   = 43,
    MX_NR_SCHED_CONTEXT_CREATE = 44,
    MX_NR_SCHED_CONTEXT_BIND_THREAD = 45,
    MX_NR_VCPU_BIND_SCHED_CONTEXT = 46,
    MX_NR_VCPU_SET_CORE_MASK = 47,
    MX_NR_VCPU_KICK = 48,
    MX_NR_VCPU_INJECT_IRQ = 49,
    MX_NR_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER = 50,

    // Driver support syscalls
    MX_NR_MAP_MMIO           = 51, // map CAP_MMIO into process VA with correct memory attrs
    MX_NR_IRQ_BIND_DOORBELL  = 52, // bind CAP_IRQ to a CAP_DOORBELL
    MX_NR_IRQ_ACK            = 53, // acknowledge IRQ (re-arm for level-triggered)
    MX_NR_CLOCK_NS           = 55, // monotonic nanoseconds for RPC deadlines
    MX_NR_CAP_INFO           = 54, // inspect trusted cap metadata into MxCapInfo

    // Phase D: PCI device assignment into a VM (spec §6, §7, §8.11/§8.12).
    MX_NR_VM_ASSIGN_DEVICE    = 56, // (vm_cap, function_cap, flags) -> assignment cap
    MX_NR_VM_ASSIGN_MAP_MMIO  = 57, // (assign_cap, bar_index, guest_ipa, size, rights, bar_off)
    MX_NR_VM_ASSIGN_ROUTE_IRQ = 58, // (assign_cap, irq_cap, vcpu_cap, guest_irq, flags)
    MX_NR_VM_ASSIGN_DESTROY   = 59, // (assign_cap) -> E_OK
    MX_NR_VM_ASSIGN_EXPOSE_ECAM = 60, // (assign_cap, guest_ipa, size) -> E_OK
    MX_NR_VM_CHANNEL_MAP = 61, // (channel, vcpu, guest_ipa, reserved_size, irq)
    MX_NR_VM_CHANNEL_NOTIFY = 62, // (channel): publish H2G wake
};

constexpr mx_u32_t MX_SPAWN_SUSPENDED = 1u << 0;
constexpr mx_u32_t MX_THREAD_SUSPENDED = 1u << 0;

// Compatibility names for existing V0 code.
constexpr mx_u64_t NR_YIELD = MX_NR_YIELD;
constexpr mx_u64_t NR_EXIT = MX_NR_EXIT;
constexpr mx_u64_t NR_SPAWN = MX_NR_SPAWN;
constexpr mx_u64_t NR_KILL = MX_NR_KILL;
constexpr mx_u64_t NR_MAP_PAGE = MX_NR_MAP_PAGE;
constexpr mx_u64_t NR_UNMAP_PAGE = MX_NR_UNMAP_PAGE;
constexpr mx_u64_t NR_ARENA_CREATE = MX_NR_ARENA_CREATE;
constexpr mx_u64_t NR_ARENA_REGISTER = MX_NR_ARENA_REGISTER;
constexpr mx_u64_t NR_CAP_DUP = MX_NR_CAP_DUP;
constexpr mx_u64_t NR_CAP_REVOKE = MX_NR_CAP_REVOKE;
constexpr mx_u64_t NR_CAP_IDENTIFY = MX_NR_CAP_IDENTIFY;
constexpr mx_u64_t NR_RING_CREATE = MX_NR_RING_CREATE;
constexpr mx_u64_t NR_RING_SHARE = MX_NR_RING_SHARE;
constexpr mx_u64_t NR_DOORBELL_CREATE = MX_NR_DOORBELL_CREATE;
constexpr mx_u64_t NR_DOORBELL_SHARE = MX_NR_DOORBELL_SHARE;
constexpr mx_u64_t NR_NOTIFY = MX_NR_NOTIFY;
constexpr mx_u64_t NR_WAIT_DOORBELL = MX_NR_WAIT_DOORBELL;
constexpr mx_u64_t NR_DEBUG_PUTS = MX_NR_DEBUG_PUTS;
constexpr mx_u64_t NR_DEBUG_COUNTERS = MX_NR_DEBUG_COUNTERS;
constexpr mx_u64_t NR_CAP_GRANT = MX_NR_CAP_GRANT;
constexpr mx_u64_t NR_DOORBELL_READ = MX_NR_DOORBELL_READ;
constexpr mx_u64_t NR_SUPERVISOR_POLL = MX_NR_SUPERVISOR_POLL;
constexpr mx_u64_t NR_THREAD_CREATE = MX_NR_THREAD_CREATE;
constexpr mx_u64_t NR_THREAD_SET_AFFINITY = MX_NR_THREAD_SET_AFFINITY;
constexpr mx_u64_t NR_THREAD_RESUME = MX_NR_THREAD_RESUME;
constexpr mx_u64_t NR_THREAD_GET_CPU = MX_NR_THREAD_GET_CPU;
constexpr mx_u64_t NR_THREAD_EXIT = MX_NR_THREAD_EXIT;
constexpr mx_u64_t NR_VM_CREATE = MX_NR_VM_CREATE;
constexpr mx_u64_t NR_VM_DESTROY = MX_NR_VM_DESTROY;
constexpr mx_u64_t NR_VM_INSPECT = MX_NR_VM_INSPECT;
constexpr mx_u64_t NR_VM_MEMORY_CREATE = MX_NR_VM_MEMORY_CREATE;
constexpr mx_u64_t NR_VM_MEMORY_MAP = MX_NR_VM_MEMORY_MAP;
constexpr mx_u64_t NR_VM_MEMORY_UNMAP = MX_NR_VM_MEMORY_UNMAP;
constexpr mx_u64_t NR_VM_MEMORY_INSPECT = MX_NR_VM_MEMORY_INSPECT;
constexpr mx_u64_t NR_VCPU_CREATE = MX_NR_VCPU_CREATE;
constexpr mx_u64_t NR_VCPU_SET_ENTRY = MX_NR_VCPU_SET_ENTRY;
constexpr mx_u64_t NR_VCPU_INSPECT = MX_NR_VCPU_INSPECT;
constexpr mx_u64_t NR_VCPU_RUN = MX_NR_VCPU_RUN;
constexpr mx_u64_t NR_VM_CHANNEL_CREATE = MX_NR_VM_CHANNEL_CREATE;
constexpr mx_u64_t NR_VM_CHANNEL_ENDPOINT = MX_NR_VM_CHANNEL_ENDPOINT;
constexpr mx_u64_t NR_VM_CHANNEL_INSPECT = MX_NR_VM_CHANNEL_INSPECT;
constexpr mx_u64_t NR_VM_MEMORY_WRITE = MX_NR_VM_MEMORY_WRITE;
constexpr mx_u64_t NR_NOTIFY_HANDOFF = MX_NR_NOTIFY_HANDOFF;
constexpr mx_u64_t NR_REPLY_HANDOFF = MX_NR_REPLY_HANDOFF;
constexpr mx_u64_t NR_SCHED_CONTEXT_CREATE = MX_NR_SCHED_CONTEXT_CREATE;
constexpr mx_u64_t NR_SCHED_CONTEXT_BIND_THREAD = MX_NR_SCHED_CONTEXT_BIND_THREAD;
constexpr mx_u64_t NR_VCPU_BIND_SCHED_CONTEXT = MX_NR_VCPU_BIND_SCHED_CONTEXT;
constexpr mx_u64_t NR_VCPU_SET_CORE_MASK = MX_NR_VCPU_SET_CORE_MASK;
constexpr mx_u64_t NR_VCPU_KICK = MX_NR_VCPU_KICK;
constexpr mx_u64_t NR_VCPU_INJECT_IRQ = MX_NR_VCPU_INJECT_IRQ;
constexpr mx_u64_t NR_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER = MX_NR_VCPU_BLOCK_UNTIL_IRQ_OR_TIMER;
constexpr mx_u64_t NR_MAP_MMIO          = MX_NR_MAP_MMIO;
constexpr mx_u64_t NR_IRQ_BIND_DOORBELL = MX_NR_IRQ_BIND_DOORBELL;
constexpr mx_u64_t NR_IRQ_ACK           = MX_NR_IRQ_ACK;
constexpr mx_u64_t NR_CAP_INFO          = MX_NR_CAP_INFO;
constexpr mx_u64_t NR_VM_ASSIGN_DEVICE    = MX_NR_VM_ASSIGN_DEVICE;
constexpr mx_u64_t NR_VM_ASSIGN_MAP_MMIO  = MX_NR_VM_ASSIGN_MAP_MMIO;
constexpr mx_u64_t NR_VM_ASSIGN_ROUTE_IRQ = MX_NR_VM_ASSIGN_ROUTE_IRQ;
constexpr mx_u64_t NR_VM_ASSIGN_DESTROY   = MX_NR_VM_ASSIGN_DESTROY;
constexpr mx_u64_t NR_VM_ASSIGN_EXPOSE_ECAM = MX_NR_VM_ASSIGN_EXPOSE_ECAM;

constexpr mx_u64_t NR_VM_CHANNEL_MAP = MX_NR_VM_CHANNEL_MAP;
constexpr mx_u64_t NR_VM_CHANNEL_NOTIFY = MX_NR_VM_CHANNEL_NOTIFY;

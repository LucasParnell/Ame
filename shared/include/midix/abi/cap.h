#pragma once
// Midix capability object types and rights.
// These constants are part of the kernel/user contract; keep in sync with anko/cap.h.

#include "midix/abi/types.h"

enum MxCapType : mx_u16_t {
    MX_CAP_NULL          = 0,
    MX_CAP_THREAD        = 1,
    MX_CAP_PROCESS       = 2,
    MX_CAP_PAGE          = 3,
    MX_CAP_SHARED_RING   = 4,
    MX_CAP_DOORBELL      = 5,
    MX_CAP_ARENA         = 6,
    MX_CAP_NAMESERVICE   = 7,
    MX_CAP_SUPERVISOR    = 8,
    MX_CAP_UART          = 9,
    MX_CAP_TIMER         = 10,
    MX_CAP_VM_ROOT       = 11,
    MX_CAP_VM            = 12,
    MX_CAP_VCPU          = 13,
    MX_CAP_VM_MEMORY     = 14,
    MX_CAP_VM_CHANNEL    = 15,
    MX_CAP_SCHED_CONTEXT = 16,
    MX_CAP_MMIO          = 17,  // device MMIO range object (phys_base, size, memory attrs)
    MX_CAP_IRQ           = 18,  // IRQ line object; driver binds to a doorbell explicitly
    MX_CAP_PCI_FUNCTION  = 19,  // host PCI function authority object (Phase D)
    MX_CAP_VM_DEVICE     = 20,  // VM device-assignment object (Phase D, spec §6.1)
    MX_CAP_PLATFORM      = 22, // narrow board mechanism authority
    MX_CAP_VM_SHARED     = 21,  // attached shared arena (Phase L, spec §12.3)
};

enum MxMmioAttr : mx_u32_t {
    MX_MMIO_DEVICE_nGnRnE = 1, // strongly ordered (xHCI registers)
    MX_MMIO_DEVICE_nGnRE = 2,  // device, early write ack (fw_cfg)
    MX_MMIO_NORMAL_NC = 3,     // normal noncacheable (framebuffer pixels)
    MX_MMIO_WRITE_COMBINE = 4, // write-combining (framebuffer if supported)
};

struct MxCapInfo {
    mx_u32_t type;
    mx_u32_t rights;
    mx_u64_t base;
    mx_u64_t size;
    mx_u32_t attr;
    mx_u32_t flags;
    mx_u64_t aux0;
};
static_assert(sizeof(MxCapInfo) == 40, "MxCapInfo must stay small");

// CAP_ARENA's attr reports the actual CPU memory type. Ordering barriers do
// not make a write-back arena suitable for a non-coherent DMA engine.
enum MxArenaMemoryAttr : mx_u32_t {
    MX_ARENA_MEMORY_WRITE_BACK = 0,
    MX_ARENA_MEMORY_NONCACHEABLE = 1,
};

// CAP_INFO's arena flags retain their low 16 bits. The upper half reports the
// saturated object reference count, excluding CAP_INFO's temporary lookup.
// A cache owning one capability and one mapping may reuse a page only at 2;
// client capabilities and mappings each keep it above that value.
constexpr mx_u32_t MX_ARENA_REFERENCE_SHIFT = 16;
constexpr mx_u32_t MX_ARENA_REFERENCE_MASK = 0xffff0000u;

enum MxRights : mx_rights_t {
    MX_RIGHT_READ     = 1u << 0,
    MX_RIGHT_WRITE    = 1u << 1,
    MX_RIGHT_MAP      = 1u << 2,
    MX_RIGHT_EXEC     = 1u << 3,
    MX_RIGHT_NOTIFY   = 1u << 4,
    MX_RIGHT_WAIT     = 1u << 5,
    MX_RIGHT_DUP      = 1u << 6,
    MX_RIGHT_SPAWN    = 1u << 7,
    MX_RIGHT_KILL     = 1u << 8,
    MX_RIGHT_REGISTER = 1u << 9,
    MX_RIGHT_THREAD_CONTROL = 1u << 10,
    MX_RIGHT_VM_CONTROL = 1u << 11,
    MX_RIGHT_VM_MAP = 1u << 12,
    MX_RIGHT_VM_INSPECT = 1u << 13,
    MX_RIGHT_VM_DESTROY = 1u << 14,
    MX_RIGHT_VCPU_CONTROL = 1u << 15,
    MX_RIGHT_VCPU_INSPECT = 1u << 16,
    MX_RIGHT_VM_MEMORY_MAP = 1u << 17,
    MX_RIGHT_VM_MEMORY_INSPECT = 1u << 18,
    MX_RIGHT_VM_CHANNEL_SEND = 1u << 19,
    MX_RIGHT_VM_CHANNEL_RECV = 1u << 20,
    MX_RIGHT_VM_CHANNEL_NOTIFY = 1u << 21,
    MX_RIGHT_VM_CHANNEL_INSPECT = 1u << 22,
    MX_RIGHT_VM_CREATE = 1u << 23,
    MX_RIGHT_SCHED_CONTEXT_CONTROL = 1u << 24,
};

constexpr mx_rights_t MX_RIGHTS_NONE = 0;
constexpr mx_rights_t MX_RIGHTS_RW = MX_RIGHT_READ | MX_RIGHT_WRITE;
constexpr mx_rights_t MX_RIGHTS_RWMAP = MX_RIGHT_READ | MX_RIGHT_WRITE | MX_RIGHT_MAP;
constexpr mx_rights_t MX_RIGHTS_RING_ENDPOINT = MX_RIGHT_READ | MX_RIGHT_WRITE | MX_RIGHT_MAP | MX_RIGHT_DUP;
constexpr mx_rights_t MX_RIGHTS_DOORBELL_ENDPOINT = MX_RIGHT_WAIT | MX_RIGHT_NOTIFY | MX_RIGHT_DUP;

#ifndef MIDIX_ABI_NO_COMPAT_NAMES
// Compatibility spellings for existing V0 code. New code should use MX_*.
constexpr mx_u32_t RIGHT_READ = MX_RIGHT_READ;
constexpr mx_u32_t RIGHT_WRITE = MX_RIGHT_WRITE;
constexpr mx_u32_t RIGHT_MAP = MX_RIGHT_MAP;
constexpr mx_u32_t RIGHT_EXEC = MX_RIGHT_EXEC;
constexpr mx_u32_t RIGHT_NOTIFY = MX_RIGHT_NOTIFY;
constexpr mx_u32_t RIGHT_WAIT = MX_RIGHT_WAIT;
constexpr mx_u32_t RIGHT_DUP = MX_RIGHT_DUP;
constexpr mx_u32_t RIGHT_SPAWN = MX_RIGHT_SPAWN;
constexpr mx_u32_t RIGHT_KILL = MX_RIGHT_KILL;
constexpr mx_u32_t RIGHT_REGISTER = MX_RIGHT_REGISTER;
constexpr mx_u32_t RIGHT_THREAD_CONTROL = MX_RIGHT_THREAD_CONTROL;
constexpr mx_u32_t RIGHT_VM_CONTROL = MX_RIGHT_VM_CONTROL;
constexpr mx_u32_t RIGHT_VM_MAP = MX_RIGHT_VM_MAP;
constexpr mx_u32_t RIGHT_VM_INSPECT = MX_RIGHT_VM_INSPECT;
constexpr mx_u32_t RIGHT_VM_DESTROY = MX_RIGHT_VM_DESTROY;
constexpr mx_u32_t RIGHT_VCPU_CONTROL = MX_RIGHT_VCPU_CONTROL;
constexpr mx_u32_t RIGHT_VCPU_INSPECT = MX_RIGHT_VCPU_INSPECT;
constexpr mx_u32_t RIGHT_VM_MEMORY_MAP = MX_RIGHT_VM_MEMORY_MAP;
constexpr mx_u32_t RIGHT_VM_MEMORY_INSPECT = MX_RIGHT_VM_MEMORY_INSPECT;
constexpr mx_u32_t RIGHT_VM_CHANNEL_SEND = MX_RIGHT_VM_CHANNEL_SEND;
constexpr mx_u32_t RIGHT_VM_CHANNEL_RECV = MX_RIGHT_VM_CHANNEL_RECV;
constexpr mx_u32_t RIGHT_VM_CHANNEL_NOTIFY = MX_RIGHT_VM_CHANNEL_NOTIFY;
constexpr mx_u32_t RIGHT_VM_CHANNEL_INSPECT = MX_RIGHT_VM_CHANNEL_INSPECT;
constexpr mx_u32_t RIGHT_VM_CREATE = MX_RIGHT_VM_CREATE;
constexpr mx_u32_t RIGHT_SCHED_CONTEXT_CONTROL = MX_RIGHT_SCHED_CONTEXT_CONTROL;

constexpr mx_i64_t kCapTypeThread = MX_CAP_THREAD;
constexpr mx_i64_t kCapTypeProcess = MX_CAP_PROCESS;
constexpr mx_i64_t kCapTypePage = MX_CAP_PAGE;
constexpr mx_i64_t kCapTypeSharedRing = MX_CAP_SHARED_RING;
constexpr mx_i64_t kCapTypeDoorbell = MX_CAP_DOORBELL;
constexpr mx_i64_t kCapTypeArena = MX_CAP_ARENA;
constexpr mx_i64_t kCapTypeNameservice = MX_CAP_NAMESERVICE;
constexpr mx_i64_t kCapTypeSupervisor = MX_CAP_SUPERVISOR;
constexpr mx_i64_t kCapTypeUart = MX_CAP_UART;
constexpr mx_i64_t kCapTypeTimer = MX_CAP_TIMER;
constexpr mx_i64_t kCapTypeVmRoot = MX_CAP_VM_ROOT;
constexpr mx_i64_t kCapTypeVm = MX_CAP_VM;
constexpr mx_i64_t kCapTypeVcpu = MX_CAP_VCPU;
constexpr mx_i64_t kCapTypeVmMemory = MX_CAP_VM_MEMORY;
constexpr mx_i64_t kCapTypeVmChannel = MX_CAP_VM_CHANNEL;
constexpr mx_i64_t kCapTypeSchedContext = MX_CAP_SCHED_CONTEXT;
constexpr mx_i64_t kCapTypeMmio = MX_CAP_MMIO;
constexpr mx_i64_t kCapTypeIrq = MX_CAP_IRQ;
constexpr mx_i64_t kCapTypePciFunction = MX_CAP_PCI_FUNCTION;
constexpr mx_i64_t kCapTypeVmDevice = MX_CAP_VM_DEVICE;
constexpr mx_i64_t kCapTypeVmShared = MX_CAP_VM_SHARED;

#endif // MIDIX_ABI_NO_COMPAT_NAMES

#if defined(__cplusplus)
// Data-oriented capability type names. One row per MxCapType accepted by the
// ABI; the shell and diagnostics read this table instead of re-spelling a
// switch. A missing row degrades to "unknown" rather than a wrong name, and
// every cap type the ABI mints has a row here.
struct MxCapTypeName {
    mx_u16_t type;
    const char* name;
};
inline constexpr MxCapTypeName kMxCapTypeNames[] = {
    {MX_CAP_NULL, "null"},
    {MX_CAP_THREAD, "thread"},
    {MX_CAP_PROCESS, "process"},
    {MX_CAP_PAGE, "page"},
    {MX_CAP_SHARED_RING, "ring"},
    {MX_CAP_DOORBELL, "doorbell"},
    {MX_CAP_ARENA, "arena"},
    {MX_CAP_NAMESERVICE, "nameservice"},
    {MX_CAP_SUPERVISOR, "supervisor"},
    {MX_CAP_UART, "uart"},
    {MX_CAP_TIMER, "timer"},
    {MX_CAP_VM_ROOT, "vm_root"},
    {MX_CAP_VM, "vm"},
    {MX_CAP_VCPU, "vcpu"},
    {MX_CAP_VM_MEMORY, "vm_memory"},
    {MX_CAP_VM_CHANNEL, "vm_channel"},
    {MX_CAP_SCHED_CONTEXT, "sched_context"},
    {MX_CAP_MMIO, "mmio"},
    {MX_CAP_IRQ, "irq"},
    {MX_CAP_PCI_FUNCTION, "pci_function"},
    {MX_CAP_VM_DEVICE, "vm_device"},
    {MX_CAP_VM_SHARED, "vm_shared"},
    {MX_CAP_PLATFORM, "platform"},
};
inline const char* mx_cap_type_name(mx_u32_t type) {
    for (const MxCapTypeName& row : kMxCapTypeNames) {
        if (row.type == type) return row.name;
    }
    return "unknown";
}
#endif // __cplusplus

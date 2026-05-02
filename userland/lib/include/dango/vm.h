#pragma once

#include <cstdint>

namespace dango {

constexpr int64_t E_PERM = -2;
constexpr int64_t E_NOTIMPL = -10;

constexpr uint32_t CAP_SHARED_RING = 4;
constexpr uint32_t CAP_PAGE = 3;
constexpr uint32_t CAP_DOORBELL = 5;
constexpr uint32_t CAP_VM_ROOT = 11;
constexpr uint32_t CAP_VM = 12;
constexpr uint32_t CAP_VCPU = 13;
constexpr uint32_t CAP_VM_MEMORY = 14;
constexpr uint32_t CAP_VM_CHANNEL = 15;

constexpr uint32_t RIGHT_READ = 1u << 0;
constexpr uint32_t RIGHT_WRITE = 1u << 1;
constexpr uint32_t RIGHT_MAP = 1u << 2;
constexpr uint32_t RIGHT_EXEC = 1u << 3;
constexpr uint32_t RIGHT_NOTIFY = 1u << 4;
constexpr uint32_t RIGHT_WAIT = 1u << 5;
constexpr uint32_t RIGHT_DUP = 1u << 6;
constexpr uint32_t RIGHT_VM_CONTROL = 1u << 11;
constexpr uint32_t RIGHT_VM_INSPECT = 1u << 13;
constexpr uint32_t RIGHT_VM_CREATE = 1u << 23;

constexpr uint32_t VM_CREATED = 1;
constexpr uint32_t VM_MEMORY_READY = 2;
constexpr uint32_t VM_VCPU_READY = 3;
constexpr uint32_t VM_READY = 4;

constexpr uint32_t VCPU_CREATED = 0;
constexpr uint32_t VCPU_CONFIGURED = 1;
constexpr uint32_t VCPU_READY = 2;

constexpr uint64_t VM_DEBUG_CREATE_CALLS = 32;
constexpr uint64_t VM_DEBUG_CREATED = 33;
constexpr uint64_t VM_DEBUG_DESTROYED = 34;
constexpr uint64_t VM_DEBUG_MEMORY_CREATED = 35;
constexpr uint64_t VM_DEBUG_MEMORY_MAPPED = 36;
constexpr uint64_t VM_DEBUG_MEMORY_UNMAPPED = 37;
constexpr uint64_t VM_DEBUG_STAGE2_TABLES_CREATED = 38;
constexpr uint64_t VM_DEBUG_STAGE2_PAGES_MAPPED = 39;
constexpr uint64_t VM_DEBUG_VCPU_CREATED = 40;
constexpr uint64_t VM_DEBUG_VCPU_CONFIGURED = 41;
constexpr uint64_t VM_DEBUG_VCPU_RUN_ATTEMPTS = 42;
constexpr uint64_t VM_DEBUG_VCPU_RUN_NOTIMPL = 43;
constexpr uint64_t VM_DEBUG_CHANNELS_CREATED = 44;
constexpr uint64_t VM_DEBUG_ERRORS = 45;
constexpr uint64_t VM_DEBUG_EL_CURRENT = 46;
constexpr uint64_t VM_DEBUG_EL2_AVAILABLE = 47;
constexpr uint64_t VM_DEBUG_VHE_AVAILABLE = 48;
constexpr uint64_t VM_DEBUG_GUEST_BOOT_STUB = 49;
constexpr uint64_t VM_DEBUG_GUEST_AGENT_START = 50;
constexpr uint64_t VM_DEBUG_GUEST_LINUX_HELLO = 51;
constexpr uint64_t VM_DEBUG_VCPU_WFX_EXITS = 52;
constexpr uint64_t VM_DEBUG_VCPU_IRQ_EXITS = 53;
constexpr uint64_t VM_DEBUG_VCPU_UNSUPPORTED_EXITS = 54;

struct VmInfo {
    uint32_t vm_id;
    uint32_t owner_pid;
    uint32_t state;
    uint32_t memory_count;
    uint32_t vcpu_count;
    uint32_t channel_count;
    uint32_t reserved;
    uint64_t guest_ram_bytes;
    uint64_t mapped_regions;
    uint64_t flags;
    uint64_t stage2_mapped_pages;
    uint64_t stage2_built;
    uint64_t stage2_active;
};

struct VmMemoryInfo {
    uint32_t mem_id;
    uint32_t owner_vm_id;
    uint32_t rights;
    uint32_t flags;
    uint64_t host_phys_base;
    uint64_t host_va;
    uint64_t size;
    uint64_t guest_pa_base;
    uint64_t mapped_size;
    uint64_t mapped_stage2;
};

struct VcpuInfo {
    uint32_t vcpu_id;
    uint32_t owner_vm_id;
    uint32_t state;
    uint32_t reserved;
    uint64_t entry_pc;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t run_count;
    uint64_t last_exit_reason;
};

struct VmChannelInfo {
    uint32_t channel_id;
    uint32_t owner_vm_id;
    uint32_t order;
    uint32_t flags;
    uint64_t messages_host_to_guest;
    uint64_t messages_guest_to_host;
    uint64_t endpoint_present_mask;
};

} // namespace dango

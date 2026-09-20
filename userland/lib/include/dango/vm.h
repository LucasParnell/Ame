#pragma once

#include <cstdint>
#include "midix/abi/status.h"
#include "midix/abi/cap.h"

namespace dango {

constexpr int64_t E_PERM = MX_ERR_PERM;
constexpr int64_t E_NOTIMPL = MX_ERR_NOT_IMPLEMENTED;

constexpr uint32_t CAP_SHARED_RING = MX_CAP_SHARED_RING;
constexpr uint32_t CAP_PAGE = MX_CAP_PAGE;
constexpr uint32_t CAP_DOORBELL = MX_CAP_DOORBELL;
constexpr uint32_t CAP_VM_ROOT = MX_CAP_VM_ROOT;
constexpr uint32_t CAP_VM = MX_CAP_VM;
constexpr uint32_t CAP_VCPU = MX_CAP_VCPU;
constexpr uint32_t CAP_VM_MEMORY = MX_CAP_VM_MEMORY;
constexpr uint32_t CAP_VM_CHANNEL = MX_CAP_VM_CHANNEL;
constexpr uint32_t CAP_SCHED_CONTEXT = MX_CAP_SCHED_CONTEXT;

constexpr uint32_t RIGHT_READ = MX_RIGHT_READ;
constexpr uint32_t RIGHT_WRITE = MX_RIGHT_WRITE;
constexpr uint32_t RIGHT_MAP = MX_RIGHT_MAP;
constexpr uint32_t RIGHT_EXEC = MX_RIGHT_EXEC;
constexpr uint32_t RIGHT_NOTIFY = MX_RIGHT_NOTIFY;
constexpr uint32_t RIGHT_WAIT = MX_RIGHT_WAIT;
constexpr uint32_t RIGHT_DUP = MX_RIGHT_DUP;
constexpr uint32_t RIGHT_VM_CONTROL = MX_RIGHT_VM_CONTROL;
constexpr uint32_t RIGHT_VM_INSPECT = MX_RIGHT_VM_INSPECT;
constexpr uint32_t RIGHT_VM_CREATE = MX_RIGHT_VM_CREATE;

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

enum SchedBand : uint32_t {
    SCHED_BAND_RT_NATIVE = 0,
    SCHED_BAND_LATENCY = 1,
    SCHED_BAND_FAIR = 2,
    SCHED_BAND_VM_LATENCY = 3,
    SCHED_BAND_VM_FAIR = 4,
    SCHED_BAND_BATCH = 5,
    SCHED_BAND_IDLE = 6,
};

constexpr uint32_t SCHED_CTX_NO_STEAL = 1u << 0;
constexpr uint32_t SCHED_CTX_VM_RUNNER = 1u << 1;
constexpr uint32_t SCHED_CTX_STEALABLE = 1u << 2;

struct SchedContext {
    uint32_t principal_id;
    uint32_t owner_thread_or_vcpu;
    uint32_t band;
    uint32_t weight;
    uint64_t budget_ns;
    uint64_t period_ns;
    uint64_t deadline_ns;
    uint64_t used_ns;
    uint64_t period_start_ns;
    uint64_t core_mask;
    uint32_t home_cpu;
    uint32_t flags;
    uint32_t id;
};

enum VcpuExitReason : uint32_t {
    VCPU_EXIT_QUANTUM_EXPIRED = 0,
    VCPU_EXIT_WFI_BLOCKED = 1,
    VCPU_EXIT_VIRTUAL_IRQ_PENDING = 2,
    VCPU_EXIT_MMIO_BATCH_READY = 3,
    VCPU_EXIT_UNSUPPORTED_TRAP = 4,
    VCPU_EXIT_SHUTDOWN = 5,
    VCPU_EXIT_HOST_PREEMPTED = 6,
};

struct VcpuRunBudget {
    uint64_t budget_ns;
    uint64_t deadline_ns;
    uint32_t max_exits;
    uint32_t flags;
};

struct VcpuRunResult {
    uint32_t exit_reason;
    uint32_t exit_batch_count;
    uint64_t last_esr;
    uint64_t used_ns;
};

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

#pragma once
// Fixed-layout VM syscall records, shared by the kernel and native clients.
#include "midix/abi/types.h"

// Stable scheduling-context request. Kernel budget synchronization is private;
// it must not change this existing 80-byte syscall record. The kernel supplies
// used_ns, period_start_ns and id when it creates the actual context.
typedef struct MxSchedContext {
    mx_u32_t principal_id;
    mx_u32_t owner_thread_or_vcpu;
    mx_u32_t band;
    mx_u32_t weight;
    mx_u64_t budget_ns;
    mx_u64_t period_ns;
    mx_u64_t deadline_ns;
    mx_u64_t used_ns;
    mx_u64_t period_start_ns;
    mx_u64_t core_mask;
    mx_u32_t home_cpu;
    mx_u32_t flags;
    mx_u32_t id;
} MxSchedContext;

#ifdef __cplusplus
static_assert(sizeof(MxSchedContext) == 80);
static_assert(__builtin_offsetof(MxSchedContext, band) == 8);
static_assert(__builtin_offsetof(MxSchedContext, used_ns) == 40);
static_assert(__builtin_offsetof(MxSchedContext, core_mask) == 56);
static_assert(__builtin_offsetof(MxSchedContext, id) == 72);
#endif

typedef struct MxVcpuRunBudget {
    mx_u64_t budget_ns;
    mx_u64_t deadline_ns;
    mx_u32_t max_exits;
    mx_u32_t flags;
} MxVcpuRunBudget;

typedef struct MxVcpuRunResult {
    mx_u32_t exit_reason;
    mx_u32_t exit_batch_count;
    mx_u64_t last_esr;
    mx_u64_t used_ns;
} MxVcpuRunResult;

typedef struct MxSharedArenaInfo {
    mx_u32_t shared_id;
    mx_u32_t owner_vm_id;
    mx_u32_t rights;
    mx_u32_t flags;
    mx_u64_t guest_pa_base;
    mx_u64_t size;
    mx_u32_t dma_class; // iommu::VmDmaClass
    mx_u32_t mapped_stage2;
    mx_u64_t host_phys_base;
} MxSharedArenaInfo;

typedef struct MxVmInfo {
    mx_u32_t vm_id;
    mx_u32_t owner_pid;
    mx_u32_t state;
    mx_u32_t memory_count;
    mx_u32_t vcpu_count;
    mx_u32_t channel_count;
    mx_u32_t reserved;
    mx_u64_t guest_ram_bytes;
    mx_u64_t mapped_regions;
    mx_u64_t flags;
    mx_u64_t stage2_mapped_pages;
    // Mapping records backing those pages: with 2 MiB blocks a fully mapped RAM
    // region reports far fewer records than pages, which is how a caller can tell
    // the block path is actually in use rather than silently falling back.
    mx_u64_t stage2_mapped_records;
    mx_u64_t stage2_built;
    mx_u64_t stage2_active;
} MxVmInfo;

typedef struct MxVmMemoryInfo {
    mx_u32_t mem_id;
    mx_u32_t owner_vm_id;
    mx_u32_t rights;
    mx_u32_t flags;
    mx_u64_t host_phys_base;
    mx_u64_t host_va;
    mx_u64_t size;
    mx_u64_t guest_pa_base;
    mx_u64_t mapped_size;
    mx_u64_t mapped_stage2;
    mx_u8_t shared_staging;
    mx_u8_t pad[7];
} MxVmMemoryInfo;

typedef struct MxVcpuInfo {
    mx_u32_t vcpu_id;
    mx_u32_t owner_vm_id;
    mx_u32_t state;
    mx_u32_t reserved;
    mx_u64_t entry_pc;
    mx_u64_t arg0;
    mx_u64_t arg1;
    mx_u64_t run_count;
    mx_u64_t last_exit_reason;
    mx_u32_t sched_context_id;
    mx_u32_t runner_tid;
    mx_u64_t core_mask;
    mx_u32_t home_cpu;
    mx_u32_t blocked_reason;
} MxVcpuInfo;

typedef struct MxVmChannelInfo {
    mx_u32_t channel_id;
    mx_u32_t owner_vm_id;
    mx_u32_t order;
    mx_u32_t flags;
    mx_u64_t messages_host_to_guest;
    mx_u64_t messages_guest_to_host;
    mx_u64_t endpoint_present_mask;
} MxVmChannelInfo;

#ifdef __cplusplus
static_assert(sizeof(MxVcpuRunBudget) == 24);
static_assert(sizeof(MxVcpuRunResult) == 24);
static_assert(sizeof(MxSharedArenaInfo) == 48);
static_assert(sizeof(MxVmInfo) == 88);
static_assert(sizeof(MxVmMemoryInfo) == 72);
static_assert(sizeof(MxVcpuInfo) == 80);
static_assert(sizeof(MxVmChannelInfo) == 40);
#endif

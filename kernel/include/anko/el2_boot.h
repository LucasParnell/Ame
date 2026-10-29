#pragma once
// Shared with entry.S: each CPU writes only its own record during startup.
// Records live in .data because the primary records its state before BSS clear.
#define ANKO_EL2_BOOT_SHIFT 6
#define ANKO_EL2_BOOT_STRIDE (1 << ANKO_EL2_BOOT_SHIFT)
#define ANKO_EL2_BOOT_ENTRY_OFFSET 0
#define ANKO_EL2_BOOT_READY_OFFSET 8

#ifndef __ASSEMBLER__
#include "anko/types.h"
#include "midix/abi/cpu_limits.h"

namespace vm {
struct alignas(ANKO_EL2_BOOT_STRIDE) El2BootCpu {
    u64 entry_el;
    u64 monitor_ready;
    u8 reserved[ANKO_EL2_BOOT_STRIDE - 2 * sizeof(u64)];
};
static_assert(sizeof(El2BootCpu) == ANKO_EL2_BOOT_STRIDE);
static_assert(__builtin_offsetof(El2BootCpu, entry_el) == ANKO_EL2_BOOT_ENTRY_OFFSET);
static_assert(__builtin_offsetof(El2BootCpu, monitor_ready) == ANKO_EL2_BOOT_READY_OFFSET);
}

extern "C" vm::El2BootCpu g_el2_boot_cpus[MX_HOST_MAX_CPUS];

namespace vm {
inline u64 entry_level(u32 cpu) {
    return cpu < MX_HOST_MAX_CPUS
        ? __atomic_load_n(&g_el2_boot_cpus[cpu].entry_el, __ATOMIC_ACQUIRE) : 0;
}

inline bool monitor_ready(u32 cpu) {
    // The assembly publishes readiness with STLR only after vector/register
    // initialization. Padding avoids false sharing during parallel CPU entry.
    return cpu < MX_HOST_MAX_CPUS &&
        __atomic_load_n(&g_el2_boot_cpus[cpu].monitor_ready, __ATOMIC_ACQUIRE) == 1;
}
}
#endif

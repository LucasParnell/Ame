#pragma once
// Read-only bring-up diagnostics. No request invokes firmware or reads EL2
// registers: hardware capability and ownership of the monitor are separate.
#include "midix/abi/types.h"

constexpr mx_u32_t MX_DIAG_VERSION = 1;
constexpr mx_u64_t MX_DIAG_READ_MAX = 256;

enum MxDiagRequest : mx_u64_t {
    MX_DIAG_SYSTEM = 0,   // index=0, MxDiagSystem; returns sizeof(MxDiagSystem)
    MX_DIAG_CPU = 1,      // index=logical CPU, MxDiagCpu; returns sizeof(MxDiagCpu)
    MX_DIAG_BOOT_LOG = 2, // index=byte offset, 1..READ_MAX bytes; returns count/EOF=0
};

enum MxDiagBoard : mx_u32_t {
    MX_DIAG_BOARD_UNKNOWN = 0,
    MX_DIAG_BOARD_QEMU = 1,
    MX_DIAG_BOARD_ANNE = 2,
    MX_DIAG_BOARD_SWITCH = 3,
};

struct MxDiagSystem {
    mx_u32_t version;
    mx_u32_t board;
    mx_u32_t cpu_limit;
    mx_u32_t sampled_cpu;
    mx_u64_t online_mask;
    mx_u64_t current_el;
    mx_u64_t midr;
    mx_u64_t mpidr;
    mx_u64_t pfr0;
    mx_u64_t mmfr0;
    mx_u64_t sctlr;
    mx_u64_t tcr;
    mx_u64_t mair;
    mx_u64_t ctr;
    mx_u64_t counter_reported_hz;
    mx_u64_t counter_effective_hz;
    // The retained log is append-only. Read only this prefix to keep a report
    // finite while other CPUs continue logging. Capacity=0 means unsupported.
    mx_u64_t boot_log_size;
    mx_u64_t boot_log_capacity;
};
static_assert(sizeof(MxDiagSystem) == 128);

struct MxDiagCpu {
    mx_u32_t version;
    mx_u32_t cpu;
    mx_u64_t online;
    mx_u64_t mpidr;
    mx_u64_t boot_entry_el; // 0 means no entry recorded, rather than EL0 entry
    mx_u64_t monitor_ready;
    mx_u64_t context_switches;
    mx_u64_t timer_preempts;
    mx_u64_t timer_ticks;
    mx_u64_t ipis_handled;
};
static_assert(sizeof(MxDiagCpu) == 72);

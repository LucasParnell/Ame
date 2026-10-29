#pragma once
#include "midix/abi/types.h"

// Fixed-size, pointer-free telemetry. Unknown measurements have no VALID bit.
constexpr mx_u32_t MX_POWER_VERSION = 2, MX_POWER_DOMAINS = 4, MX_POWER_CPUS = 8;
constexpr mx_u32_t MX_POWER_BATTERY_MV = 1, MX_POWER_CHARGER_VALID = 2,
                   MX_POWER_BATTERY_CURRENT_NOMINAL = 4;
constexpr mx_u32_t MX_CHARGER_INPUT = 1, MX_CHARGER_ENABLED = 2, MX_CHARGER_DONE = 4,
                   MX_CHARGER_SOURCE = 8, MX_CHARGER_THERMAL = 16;
constexpr mx_u32_t MX_FREQ_OBSERVED = 1, MX_FREQ_CONTROL = 2, MX_FREQ_LOAD = 4, MX_FREQ_THERMAL = 8;
struct MxPowerCpu {
    mx_u64_t idle_ns, idle_entries;
};
struct MxPowerDomain {
    mx_u64_t cpu_mask;
    mx_u32_t flags, opp_count, min_khz, max_khz;
    mx_u32_t observed_khz, target_khz, thermal_cap_khz, busy_permille;
    mx_u32_t applied_khz, transitions, failures;
    mx_i32_t control_status;
};
struct MxPowerThermal {
    mx_u64_t sampled_ns;
    mx_i32_t millicelsius;
    mx_u32_t valid;
};
struct MxPowerSnapshot {
    mx_u32_t version, flags;
    mx_u64_t sampled_ns, online_mask, battery_sampled_ns, charger_sampled_ns;
    mx_u32_t battery_mv, charger_flags, domain_count, idle_valid_mask;
    mx_i32_t battery_ua; // nominal 10 mOhm calibration; positive=discharging
    mx_u32_t reserved;
    MxPowerCpu cpus[MX_POWER_CPUS];
    MxPowerDomain domains[MX_POWER_DOMAINS];
    // cluster0, cluster1, GPU, battery. Explicit validity and age; no fake 0 C.
    MxPowerThermal thermals[4];
};
static_assert(sizeof(MxPowerSnapshot) <= 1024);

#pragma once
#include "anko/types.h"
#include "anko/syscall.h"
#include "anko/cpu_topology.h"
#include "midix/abi/power.h"
namespace power {
// Boot-only publication, before secondaries are released.
void init(PhysAddr dtb, const smp::CpuTopology& topology);
SysError system_power_check(u32 operation);
SysError system_off();
SysError system_reset(bool fastboot = false);
void activity_init(u64 now = 0);
u64 activity_started_ns();
void idle_enter(u32 cpu, u64 now); // owning CPU, IRQs masked
void idle_leave(u32 cpu, u64 now);
bool cpu_idle(u32 cpu, u64 now, MxPowerCpu& out);
void snapshot(MxPowerSnapshot& out);
void thermal_init(PhysAddr dtb);
void thermal_sample(u64 now);
void battery_sample(u64 now, u32& millivolts, u64& sampled_ns);
SysError charger_publish(u64 packed);
void charger_invalidate();
void frequency_init(PhysAddr dtb, const smp::CpuTopology& topology);
void frequency_tick(u64 now, u64 online);
void frequency_snapshot(u64 now, u64 online, MxPowerDomain* out, u32& count);
// Cold control path only. Backends own voltage/clock ordering and completion.
using FrequencySet = SysError (*)(u32 domain, u32 khz);
bool frequency_backend(FrequencySet set); // boot only, nullptr disables control
SysError frequency_limits(u32 domain, u32 min_khz, u32 max_khz);
SysError frequency_thermal_cap(u32 domain, u32 max_khz); // 0 removes cap
SysError frequency_request(u32 domain, u32 khz);
bool thermal_snapshot(u64 now, MxPowerThermal* out);
// Sensor providers publish calibrated milli-Celsius; false invalidates a sample.
SysError thermal_publish(u32 sensor, i32 millicelsius, bool valid, u64 now);
} // namespace power

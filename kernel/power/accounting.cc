#include "anko/power.h"
#include "libk/idle_account.h"
namespace power {
namespace {
libk::IdleAccount activity[MX_POWER_CPUS];
u64 epoch;
} // namespace
void activity_init(u64 now) {
    epoch = now;
    for (auto& cpu : activity)
        cpu.reset();
}
u64 activity_started_ns() {
    return epoch;
}
void idle_enter(u32 cpu, u64 now) {
    if (cpu < MX_POWER_CPUS)
        activity[cpu].enter(now);
}
void idle_leave(u32 cpu, u64 now) {
    if (cpu < MX_POWER_CPUS)
        activity[cpu].leave(now);
}
bool cpu_idle(u32 cpu, u64 now, MxPowerCpu& out) {
    if (cpu >= MX_POWER_CPUS)
        return false;
    std::uint64_t idle = 0, count = 0;
    if (!activity[cpu].read(now, idle, count))
        return false;
    out.idle_ns = idle;
    out.idle_entries = count;
    return true;
}
} // namespace power

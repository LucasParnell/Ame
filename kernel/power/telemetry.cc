#include "anko/power.h"
#include "anko/sched.h"
#include "anko/spinlock.h"
#include "libk/bytes.h"
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
#include "anne/battery.h"
#endif
namespace power {
namespace {
kernel::SpinLock telemetry_lock;
u64 battery_attempt, battery_time, charger_time;
u32 battery_mv, charger_flags;
i32 battery_ua;
bool current_valid;
bool attempted, charger_valid;
MxPowerThermal thermals[4]{};
} // namespace
void battery_sample(u64 now, u32& millivolts, u64& sampled_ns) {
    kernel::LockGuard guard(telemetry_lock);
    if (!attempted || (now >= battery_attempt && now - battery_attempt >= 1000000000)) {
        attempted = true;
        battery_attempt = now;
        battery_mv = 0;
        battery_time = 0;
        current_valid = false;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
        const libk::Mmio pmic{reinterpret_cast<volatile u8*>(0xfff34000), 4096};
        if (anne::battery::voltage_mv(pmic, battery_mv))
            battery_time = now;
        current_valid = anne::battery::current_ua(pmic, battery_ua);
#endif
    }
    millivolts = battery_mv;
    sampled_ns = battery_time;
}
SysError charger_publish(u64 packed) {
    // Value is a verified STATUS0 byte plus CHG_REG0 byte, not a claim of SOC
    // or measured battery current. Reserved bits above the two bytes must be 0.
    if (packed >> 16)
        return E_INVALID;
    // The charger owner already runs a one-second maintenance loop. Use that
    // cold process context for EL3 sensors, so IRQ/dispatch paths never issue SMC.
    thermal_sample(sched::sched_clock_ns());
    kernel::LockGuard guard(telemetry_lock);
    const u32 status = packed & 255, config = (packed >> 8) & 255;
    charger_flags = 0;
    if (status & 0x80)
        charger_flags |= MX_CHARGER_INPUT;
    if (config & 0x20)
        charger_flags |= MX_CHARGER_ENABLED;
    if ((status & 0x30) == 0x30)
        charger_flags |= MX_CHARGER_DONE;
    if (status & 0x40)
        charger_flags |= MX_CHARGER_SOURCE;
    if (status & 2)
        charger_flags |= MX_CHARGER_THERMAL;
    charger_time = sched::sched_clock_ns();
    charger_valid = true;
    return E_OK;
}
void charger_invalidate() {
    kernel::LockGuard guard(telemetry_lock);
    charger_valid = false;
    charger_flags = 0;
    charger_time = 0;
}
SysError thermal_publish(u32 sensor, i32 temperature, bool valid, u64 now) {
    if (sensor >= 4 || (valid && (temperature < -40000 || temperature > 150000)))
        return E_INVALID;
    kernel::LockGuard guard(telemetry_lock);
    thermals[sensor] = {now, valid ? temperature : 0, valid ? 1u : 0u};
    return E_OK;
}
bool thermal_snapshot(u64 now, MxPowerThermal* out) {
    kernel::LockGuard guard(telemetry_lock);
    bool any = false;
    for (u32 i = 0; i < 4; ++i) {
        out[i] = thermals[i];
        if (now < out[i].sampled_ns || now - out[i].sampled_ns > 2000000000)
            out[i].valid = 0;
        any |= out[i].valid != 0;
    }
    return any;
}
void snapshot(MxPowerSnapshot& out) {
    libk::zero(out);
    out.version = MX_POWER_VERSION;
    out.sampled_ns = sched::sched_clock_ns();
    out.online_mask = sched::online_cpu_mask();
    thermal_sample(out.sampled_ns);
    battery_sample(out.sampled_ns, out.battery_mv, out.battery_sampled_ns);
    if (out.battery_mv)
        out.flags |= MX_POWER_BATTERY_MV;
    {
        kernel::LockGuard guard(telemetry_lock);
        if (current_valid) {
            out.battery_ua = battery_ua;
            out.flags |= MX_POWER_BATTERY_CURRENT_NOMINAL;
        }
        if (charger_valid && out.sampled_ns >= charger_time &&
            out.sampled_ns - charger_time <= 3000000000) {
            out.flags |= MX_POWER_CHARGER_VALID;
            out.charger_flags = charger_flags;
            out.charger_sampled_ns = charger_time;
        }
        for (u32 i = 0; i < 4; ++i) {
            out.thermals[i] = thermals[i];
            if (out.sampled_ns < thermals[i].sampled_ns ||
                out.sampled_ns - thermals[i].sampled_ns > 2000000000)
                out.thermals[i].valid = 0;
        }
    }
    for (u32 cpu = 0; cpu < MX_POWER_CPUS; ++cpu)
        if ((out.online_mask & (u64{1} << cpu)) && cpu_idle(cpu, out.sampled_ns, out.cpus[cpu]))
            out.idle_valid_mask |= 1u << cpu;
    frequency_snapshot(out.sampled_ns, out.online_mask, out.domains, out.domain_count);
}
} // namespace power

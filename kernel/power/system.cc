// System transitions live here; SMP owns CPU bring-up and wake IPIs.
#include "anko/power.h"
#include "anko/platform_clock.h"
#include "anko/platform.h"
#include "anko/mm.h"
#include "anko/sched.h"
#include "anko/log.h"
#include "anko/interrupt_mask.h"
#include "midix/abi/platform.h"
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
#include "../../boards/huawei-anne/boot_diagnostics.h"
#include "../../boards/huawei-anne/debug_io.h"
#include "anne/system.h"
#endif
extern "C" u64 psci_hvc_call(u64, u64, u64, u64);
extern "C" u64 psci_smc_call(u64, u64, u64, u64);
namespace power {
namespace {
smp::CpuTopology firmware{};
[[maybe_unused]] u64 counter() {
    u64 value;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(value));
    return value;
}
[[maybe_unused]] u64 psci_call(u64 fn, u64 a, u64 b, u64 c) {
    return firmware.psci_hvc ? psci_hvc_call(fn, a, b, c) : psci_smc_call(fn, a, b, c);
}
} // namespace
void init(PhysAddr dtb, const smp::CpuTopology& topology) {
    firmware = topology;
    const auto started = sched::sched_clock_ns();
    frequency_init(dtb, topology);
    const auto frequency_done = sched::sched_clock_ns();
    thermal_init(dtb);
    const auto discovery_done = sched::sched_clock_ns();
    thermal_sample(sched::sched_clock_ns());
    const auto sensors_done = sched::sched_clock_ns();
    frequency_tick(sched::sched_clock_ns(), 1);
    const auto governor_done = sched::sched_clock_ns();

    ANKO_LOGF_INFO("kernel/power",
                   "boot profile frequency=%llu thermal_dt=%llu sensors=%llu governor=%llu",
                   static_cast<unsigned long long>(frequency_done - started),
                   static_cast<unsigned long long>(discovery_done - frequency_done),
                   static_cast<unsigned long long>(sensors_done - discovery_done),
                   static_cast<unsigned long long>(governor_done - sensors_done));
}
SysError system_power_check(u32 operation) {
    if (operation != MX_PLATFORM_SYSTEM_OFF && operation != MX_PLATFORM_SYSTEM_RESET &&
        operation != MX_PLATFORM_SYSTEM_FASTBOOT)
        return E_INVALID;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    return E_OK;
#else
    if (operation == MX_PLATFORM_SYSTEM_FASTBOOT)
        return E_NOTIMPL;
    const u64 function =
        operation == MX_PLATFORM_SYSTEM_OFF ? firmware.system_off : firmware.system_reset;
    return firmware.psci_available && function ? E_OK : E_NOTIMPL;
#endif
}

namespace {
struct PowerIrqGuard {
    u64 saved;
    PowerIrqGuard() {
        asm volatile("mrs %0, daif; msr daifset, #3" : "=r"(saved)::"memory");
    }
    ~PowerIrqGuard() {
        asm volatile("msr daif, %0" ::"r"(kernel::host_interrupt_state(saved)) : "memory");
    }
};
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
void power_wait(u32 seconds) {
    asm volatile("dsb sy" ::: "memory");
    u64 reported;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(reported));
    const u64 ticks = platform::counter_frequency(reported) * seconds;
    const u64 start = counter();
    while (counter() - start < ticks)
        asm volatile("yield" ::: "memory");
}
SysError native_reset(u8 reason) {
    const libk::Mmio pmic{reinterpret_cast<volatile u8*>(anne::kDebugPmicBase), PAGE_SIZE};
    const libk::Mmio sctrl{reinterpret_cast<volatile u8*>(anne::kDebugSctrlBase), PAGE_SIZE};
    if (!anne::power::set_reason(pmic, reason))
        return E_FAULT;
    anne::boot_log_flush();
    if (!anne::power::pulse_reset(sctrl))
        return E_FAULT;
    // A normal reset uses one ordered pulse, not the fault handler's endless
    // retry loop. If firmware refuses, restore interrupts and return an error.
    power_wait(3);
    return E_BUSY;
}
#endif
} // namespace

SysError system_off() {
    const auto supported = system_power_check(MX_PLATFORM_SYSTEM_OFF);
    if (supported != E_OK)
        return supported;
    PowerIrqGuard guard;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    const libk::Mmio pmic{reinterpret_cast<volatile u8*>(anne::kDebugPmicBase), PAGE_SIZE};
    const libk::Mmio gpio{reinterpret_cast<volatile u8*>(anne::power::kPowerholdBase), PAGE_SIZE};
    if (!anne::power::set_reason(pmic, anne::power::kReasonOff))
        return E_FAULT;
    anne::boot_log_flush();
    if (!anne::power::lower_powerhold(gpio))
        return E_FAULT;
    power_wait(1);
    // Match the vendor path: external power can keep the PMIC alive. Request
    // its charger boot mode after the power-hold attempt instead of hanging.
    return native_reset(anne::power::kReasonCharger);
#else
    const auto result = psci_call(firmware.system_off, 0, 0, 0);
    ANKO_LOGF_ERROR("kernel/power", "PSCI system_off returned rc=%llx", (unsigned long long)result);
    return E_NOTIMPL;
#endif
}

SysError system_reset(bool fastboot) {
    const auto supported =
        system_power_check(fastboot ? MX_PLATFORM_SYSTEM_FASTBOOT : MX_PLATFORM_SYSTEM_RESET);
    if (supported != E_OK)
        return supported;
    PowerIrqGuard guard;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    return native_reset(fastboot ? anne::power::kReasonFastboot : anne::power::kReasonNormal);
#else
    const auto result = psci_call(firmware.system_reset, 0, 0, 0);
    ANKO_LOGF_ERROR("kernel/power", "PSCI system_reset returned rc=%llx",
                    (unsigned long long)result);
    return E_NOTIMPL;
#endif
}

} // namespace power

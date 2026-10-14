// kernel/smp.cc — DT-described PSCI SMP bring-up and Ressha IPI handling.

#include "anko/smp.h"
#include "anko/mm.h"
#include "anko/hw_info.h"
#include "anko/platform.h"
#include "anko/sched.h"
#include "anko/log.h"
#include "libk/printf.h"
#include "libk/string.h"
#include "anko/cpu_topology.h"
#include "anko/platform_clock.h"
#include "anko/el2_boot.h"
#include "anko/interrupt_mask.h"
#include "midix/abi/platform.h"
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "boot_diagnostics.h"
#include "debug_io.h"
#include "power.h"
#endif

namespace drivers::gic { void init_cpu(); u32 cpu_target(u32 cpu); void send_sgi(u32 cpu_id, u32 sgi_id); }
namespace drivers::timer { void init_cpu(u32 irq_id); }

extern "C" void vectors();
extern "C" void secondary_start();
extern "C" char __text_start[];
extern "C" char __kernel_end[];
extern "C" u64 g_boot_el;
extern "C" u64 psci_hvc_call(u64 fn, u64 a0, u64 a1, u64 a2);
extern "C" u64 psci_smc_call(u64 fn, u64 a0, u64 a1, u64 a2);
extern "C" { u64 g_cpu_mpidrs[MX_HOST_MAX_CPUS]{}; }

namespace smp {

namespace {

constexpr u32 kWakeSgi = 1;
static CpuTopology g_info{};
static u32 g_cpu_started[kMaxCpus]{};
static u32 g_release_secondaries = 0;

static u64 counter() {
    u64 now; asm volatile("isb; mrs %0, cntpct_el0" : "=r"(now)); return now;
}
static void clean_kernel_for_secondary_entry() {
    u64 ctr; asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = reinterpret_cast<u64>(__text_start) & ~(line - 1);
         p < reinterpret_cast<u64>(__kernel_end); p += line)
        asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

static u64 psci_call(u64 fn, u64 a0, u64 a1, u64 a2) {
    if (g_info.psci_hvc) return psci_hvc_call(fn, a0, a1, a2);
    else                  return psci_smc_call(fn, a0, a1, a2);
}

} // namespace

void init(PhysAddr dtb_phys) {
    u64 boot_mpidr; asm volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    g_info = read_cpu_topology(dtb_phys, boot_mpidr);
    for (u32 cpu = 0; cpu < MX_HOST_MAX_CPUS; ++cpu)
        g_cpu_mpidrs[cpu] = cpu < g_info.count ? g_info.mpidr[cpu] : ~0ull;
    __atomic_store_n(&g_cpu_started[0], 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release_secondaries, 0u, __ATOMIC_RELEASE);
    u64 reported; asm volatile("mrs %0, cntfrq_el0" : "=r"(reported));
    const u64 timeout = platform::counter_frequency(reported) * 2;
    ANKO_LOGF_INFO("kernel/smp", "topology cpus=%u conduit=%s cpu_on=%llx boot=%llx", g_info.count,
                  g_info.psci_hvc ? "hvc" : "smc", (unsigned long long)g_info.cpu_on,
                  (unsigned long long)g_info.mpidr[0]);
    clean_kernel_for_secondary_entry();
    for (u32 cpu = 1; g_info.psci_available && cpu < g_info.count; ++cpu) {
        const i64 rc = static_cast<i64>(psci_call(g_info.cpu_on, g_info.mpidr[cpu],
                          reinterpret_cast<u64>(&secondary_start), cpu));
        ANKO_LOGF_INFO("kernel/smp", "cpu%u mpidr=%llx CPU_ON rc=%lld", cpu,
                      (unsigned long long)g_info.mpidr[cpu], (long long)rc);
        if (rc != 0) continue;
        const u64 start = counter();
        while (!__atomic_load_n(&g_cpu_started[cpu], __ATOMIC_ACQUIRE) && counter() - start < timeout)
            asm volatile("yield" ::: "memory");
        if (!__atomic_load_n(&g_cpu_started[cpu], __ATOMIC_ACQUIRE))
            ANKO_LOGF_WARN("kernel/smp", "cpu%u entry timed out", cpu);
    }
    ANKO_LOGF_INFO("kernel/smp", "smp ready online=%u mask=%llx", sched::online_cpu_count(),
                  (unsigned long long)sched::online_cpu_mask());
    __atomic_store_n(&g_release_secondaries, 1u, __ATOMIC_RELEASE);
    asm volatile("sev" ::: "memory");
}

u32 cpu_count() {
    return sched::online_cpu_count();
}

SysError system_power_check(u32 operation) {
    if (operation != MX_PLATFORM_SYSTEM_OFF && operation != MX_PLATFORM_SYSTEM_RESET &&
        operation != MX_PLATFORM_SYSTEM_FASTBOOT) return E_INVALID;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    return E_OK;
#else
    if (operation == MX_PLATFORM_SYSTEM_FASTBOOT) return E_NOTIMPL;
    const u64 function = operation == MX_PLATFORM_SYSTEM_OFF ? g_info.system_off : g_info.system_reset;
    return g_info.psci_available && function ? E_OK : E_NOTIMPL;
#endif
}

namespace {
struct PowerIrqGuard {
    u64 saved;
    PowerIrqGuard() { asm volatile("mrs %0, daif; msr daifset, #3" : "=r"(saved) :: "memory"); }
    ~PowerIrqGuard() {
        asm volatile("msr daif, %0" :: "r"(kernel::host_interrupt_state(saved)) : "memory");
    }
};
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
void power_wait(u32 seconds) {
    asm volatile("dsb sy" ::: "memory");
    u64 reported; asm volatile("mrs %0, cntfrq_el0" : "=r"(reported));
    const u64 ticks = platform::counter_frequency(reported) * seconds;
    const u64 start = counter();
    while (counter() - start < ticks) asm volatile("yield" ::: "memory");
}
SysError native_reset(u8 reason) {
    const libk::Mmio pmic{reinterpret_cast<volatile u8*>(anne::kDebugPmicBase), PAGE_SIZE};
    const libk::Mmio sctrl{reinterpret_cast<volatile u8*>(anne::kDebugSctrlBase), PAGE_SIZE};
    if (!anne::power::set_reason(pmic, reason)) return E_FAULT;
    anne::boot_log_flush();
    if (!anne::power::pulse_reset(sctrl)) return E_FAULT;
    // A normal reset uses one ordered pulse, not the fault handler's endless
    // retry loop. If firmware refuses, restore interrupts and return an error.
    power_wait(3);
    return E_BUSY;
}
#endif
}

SysError system_off() {
    const auto supported = system_power_check(MX_PLATFORM_SYSTEM_OFF);
    if (supported != E_OK) return supported;
    PowerIrqGuard guard;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    const libk::Mmio pmic{reinterpret_cast<volatile u8*>(anne::kDebugPmicBase), PAGE_SIZE};
    const libk::Mmio gpio{reinterpret_cast<volatile u8*>(anne::power::kPowerholdBase), PAGE_SIZE};
    if (!anne::power::set_reason(pmic, anne::power::kReasonOff)) return E_FAULT;
    anne::boot_log_flush();
    if (!anne::power::lower_powerhold(gpio)) return E_FAULT;
    power_wait(1);
    // Match the vendor path: external power can keep the PMIC alive. Request
    // its charger boot mode after the power-hold attempt instead of hanging.
    return native_reset(anne::power::kReasonCharger);
#else
    const auto result = psci_call(g_info.system_off, 0, 0, 0);
    ANKO_LOGF_ERROR("kernel/smp", "PSCI system_off returned rc=%llx", (unsigned long long)result);
    return E_NOTIMPL;
#endif
}

SysError system_reset(bool fastboot) {
    const auto supported = system_power_check(fastboot ? MX_PLATFORM_SYSTEM_FASTBOOT : MX_PLATFORM_SYSTEM_RESET);
    if (supported != E_OK) return supported;
    PowerIrqGuard guard;
#if defined(ANKO_BOARD_HUAWEI_ANNE) && !defined(ANKO_ANNE_DIAG_TEST)
    return native_reset(fastboot ? anne::power::kReasonFastboot : anne::power::kReasonNormal);
#else
    const auto result = psci_call(g_info.system_reset, 0, 0, 0);
    ANKO_LOGF_ERROR("kernel/smp", "PSCI system_reset returned rc=%llx", (unsigned long long)result);
    return E_NOTIMPL;
#endif
}

void send_wake_ipi(u32 cpu_id) {
    sched::note_ipi_sent(cpu_id);
    drivers::gic::send_sgi(cpu_id, kWakeSgi);
    asm volatile("sev" ::: "memory");
}

void handle_ipi() {
    sched::note_ipi_handled();
    sched::drain_pending_wakes();
    asm volatile("sev" ::: "memory");
}

extern "C" [[noreturn]] void secondary_kernel_main(u64 cpu_id) {
    if (cpu_id >= kMaxCpus) for (;;) asm volatile("wfe");
    asm volatile("msr TPIDR_EL1, %0\n"
                 "isb\n"
                 :
                 : "r"(cpu_id)
                 : "memory");
    mm::vm_enable_kernel();
#ifdef ANKO_BOARD_HUAWEI_ANNE
    anne::boot_secondary_online(static_cast<u32>(cpu_id), g_info.mpidr[cpu_id]);
#endif
    asm volatile(
        "msr VBAR_EL1, %0\n"
        "isb\n"
        :
        : "r"(&vectors)
        : "memory");
    drivers::gic::init_cpu();
    drivers::timer::init_cpu(g_hw.timer_irq);
    ANKO_LOGF_INFO("kernel/smp", "cpu%u el2_entry=%llu el2_monitor=%u", (u32)cpu_id,
                  (unsigned long long)vm::entry_level((u32)cpu_id),
                  vm::monitor_ready((u32)cpu_id) ? 1u : 0u);
    ANKO_LOGF_INFO("kernel/smp", "cpu%u local GIC target=%x timer ready", (u32)cpu_id, drivers::gic::cpu_target(cpu_id));
    sched::mark_cpu_online(static_cast<u32>(cpu_id));
    __atomic_store_n(&g_cpu_started[cpu_id], 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_release_secondaries, __ATOMIC_ACQUIRE)) asm volatile("wfe" ::: "memory");
    sched::enter_secondary(static_cast<u32>(cpu_id));
}

} // namespace smp

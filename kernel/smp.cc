// kernel/smp.cc — DT-described PSCI SMP bring-up and Ressha IPI handling.

#include "anko/smp.h"
#include "libk/wait.h"
#include "anko/power.h"
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
#ifdef ANKO_ANNE_WATCHDOG
#include "watchdog.h"
#endif
#endif

namespace drivers::gic {
void init_cpu();
u32 cpu_target(u32 cpu);
void send_sgi(u32 cpu_id, u32 sgi_id);
} // namespace drivers::gic
namespace drivers::timer {
void init_cpu(u32 irq_id);
}

extern "C" void vectors();
extern "C" void secondary_start();
extern "C" char __text_start[];
extern "C" char __kernel_end[];
extern "C" u64 g_boot_el;
extern "C" u64 psci_hvc_call(u64 fn, u64 a0, u64 a1, u64 a2);
extern "C" u64 psci_smc_call(u64 fn, u64 a0, u64 a1, u64 a2);
extern "C" {
u64 g_cpu_mpidrs[MX_HOST_MAX_CPUS]{};
}

namespace smp {

namespace {

constexpr u32 kWakeSgi = 1;
static CpuTopology g_info{};
static u32 g_cpu_started[kMaxCpus]{};
static u32 g_release_secondaries = 0;
static u64 g_rendezvous_epoch;
static u64 g_rendezvous_request;
static u64 g_rendezvous_targets;
static u64 g_rendezvous_ack[kMaxCpus];

static u64 counter() {
    u64 now;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
    return now;
}
static void clean_kernel_for_secondary_entry() {
    u64 ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const u64 line = 4ull << ((ctr >> 16) & 15);
    for (u64 p = reinterpret_cast<u64>(__text_start) & ~(line - 1);
         p < reinterpret_cast<u64>(__kernel_end); p += line)
        asm volatile("dc cvac, %0" ::"r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

static u64 psci_call(u64 fn, u64 a0, u64 a1, u64 a2) {
    if (g_info.psci_hvc)
        return psci_hvc_call(fn, a0, a1, a2);
    else
        return psci_smc_call(fn, a0, a1, a2);
}

} // namespace

void init(PhysAddr dtb_phys) {
    const auto started = sched::sched_clock_ns();
    u64 boot_mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    g_info = read_cpu_topology(dtb_phys, boot_mpidr);
    const auto topology_done = sched::sched_clock_ns();
    power::init(dtb_phys, g_info);
    const auto power_done = sched::sched_clock_ns();
    for (u32 cpu = 0; cpu < MX_HOST_MAX_CPUS; ++cpu)
        g_cpu_mpidrs[cpu] = cpu < g_info.count ? g_info.mpidr[cpu] : ~0ull;
    __atomic_store_n(&g_cpu_started[0], 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_release_secondaries, 0u, __ATOMIC_RELEASE);
    u64 reported;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(reported));
    const u64 timeout = platform::counter_frequency(reported) * 2;
    ANKO_LOGF_INFO("kernel/smp", "topology cpus=%u conduit=%s cpu_on=%llx boot=%llx", g_info.count,
                   g_info.psci_hvc ? "hvc" : "smc", (unsigned long long)g_info.cpu_on,
                   (unsigned long long)g_info.mpidr[0]);
    clean_kernel_for_secondary_entry();
    const auto clean_done = sched::sched_clock_ns();
    auto wait_for = [&](u64 targets) {
        const u64 wait_started = counter();
        auto pending = [&] {
            u64 waiting = targets;
            for (u32 cpu = 1; cpu < g_info.count; ++cpu) {
                if (__atomic_load_n(&g_cpu_started[cpu], __ATOMIC_ACQUIRE))
                    waiting &= ~(u64{1} << cpu);
            }
            return waiting;
        };
        while (pending() && counter() - wait_started < timeout) {
            // IRQs are masked here. WFE cannot enforce a deadline when a
            // requested CPU never signals arrival.
#if defined(ANKO_BOARD_HUAWEI_ANNE) && defined(ANKO_ANNE_WATCHDOG)
            anne::watchdog::poll();
#endif
            libk::cpu::relax();
        }
    };
    // Retain the verified sequential startup order on Anne's vendor firmware.
    // Other platforms can initialize secondaries concurrently.
    u64 requested = 0;
    for (u32 cpu = 1; g_info.psci_available && cpu < g_info.count; ++cpu) {
        const i64 rc = static_cast<i64>(psci_call(g_info.cpu_on, g_info.mpidr[cpu],
                                                  mm::kernel_address_to_phys(reinterpret_cast<u64>(&secondary_start)), cpu));
        if (rc == 0) {
            requested |= u64{1} << cpu;
#ifdef ANKO_BOARD_HUAWEI_ANNE
            wait_for(u64{1} << cpu);
#endif
        } else
            ANKO_LOGF_WARN("kernel/smp", "cpu%u CPU_ON rc=%lld", cpu, (long long)rc);
    }
#ifndef ANKO_BOARD_HUAWEI_ANNE
    wait_for(requested);
#endif

    for (u32 cpu = 1; cpu < g_info.count; ++cpu) {
        if (!(requested & (u64{1} << cpu)))
            continue;
        if (!__atomic_load_n(&g_cpu_started[cpu], __ATOMIC_ACQUIRE))
            ANKO_LOGF_WARN("kernel/smp", "cpu%u entry timed out", cpu);
    }
    const auto cpus_done = sched::sched_clock_ns();
    ANKO_LOGF_INFO("kernel/smp", "boot profile topology=%llu power=%llu clean=%llu cpus=%llu",
                   static_cast<unsigned long long>(topology_done - started),
                   static_cast<unsigned long long>(power_done - topology_done),
                   static_cast<unsigned long long>(clean_done - power_done),
                   static_cast<unsigned long long>(cpus_done - clean_done));
    ANKO_LOGF_INFO("kernel/smp", "smp ready online=%u mask=%llx", sched::online_cpu_count(),
                   (unsigned long long)sched::online_cpu_mask());
    __atomic_store_n(&g_release_secondaries, 1u, __ATOMIC_RELEASE);
    asm volatile("sev" ::: "memory");
}

u32 cpu_count() {
    return sched::online_cpu_count();
}

void send_wake_ipi(u32 cpu_id) {
    sched::note_ipi_sent(cpu_id);
    drivers::gic::send_sgi(cpu_id, kWakeSgi);
    asm volatile("sev" ::: "memory");
}

void handle_ipi() {
    const u32 cpu = sched::current_cpu_id();
    const u64 epoch = __atomic_load_n(&g_rendezvous_request, __ATOMIC_ACQUIRE);
    if (epoch && (__atomic_load_n(&g_rendezvous_targets, __ATOMIC_RELAXED) & (1ull << cpu))) {
        // IRQ entry is a quiescent boundary: interrupt-masked kernel accesses
        // and lock holders must finish before this handler can acknowledge.
        asm volatile("dsb sy" ::: "memory");
        __atomic_store_n(&g_rendezvous_ack[cpu], epoch, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_rendezvous_request, __ATOMIC_ACQUIRE) == epoch)
            asm volatile("yield" ::: "memory");
        asm volatile("dsb ish; isb" ::: "memory");
    }
    sched::note_ipi_handled();
    sched::drain_pending_wakes();
    asm volatile("sev" ::: "memory");
}

bool rendezvous() {
    const u64 targets = sched::online_cpu_mask() & ~(1ull << sched::current_cpu_id());
    const u64 epoch = ++g_rendezvous_epoch;
    __atomic_store_n(&g_rendezvous_targets, targets, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rendezvous_request, epoch, __ATOMIC_RELEASE);
    for (u32 cpu = 0; cpu < kMaxCpus; ++cpu)
        if (targets & (1ull << cpu)) send_wake_ipi(cpu);
    const u64 deadline = sched::sched_clock_ns() + 10'000'000; // bounded 10 ms
    for (;;) {
        bool ready = true;
        for (u32 cpu = 0; cpu < kMaxCpus; ++cpu)
            if ((targets & (1ull << cpu)) &&
                __atomic_load_n(&g_rendezvous_ack[cpu], __ATOMIC_ACQUIRE) != epoch) ready = false;
        if (ready) return true;
        if (sched::sched_clock_ns() >= deadline) {
            resume();
            return false;
        }
        asm volatile("yield" ::: "memory");
    }
}

void resume() {
    __atomic_store_n(&g_rendezvous_request, 0ull, __ATOMIC_RELEASE);
    asm volatile("sev" ::: "memory");
}

extern "C" [[noreturn]] void secondary_kernel_main(u64 cpu_id) {
    if (cpu_id >= kMaxCpus)
        for (;;)
            asm volatile("wfe");
    asm volatile("msr TPIDR_EL1, %0\n"
                 "isb\n"
                 :
                 : "r"(cpu_id)
                 : "memory");
    mm::vm_enable_kernel();
#ifdef ANKO_BOARD_HUAWEI_ANNE
    anne::boot_secondary_online(static_cast<u32>(cpu_id), g_info.mpidr[cpu_id]);
#endif
    asm volatile("msr VBAR_EL1, %0\n"
                 "isb\n"
                 :
                 : "r"(&vectors)
                 : "memory");
    drivers::gic::init_cpu();
    drivers::timer::init_cpu(g_hw.timer_irq);
    ANKO_LOGF_VERBOSE("kernel/smp", "cpu%u entry=%llu monitor=%u", (u32)cpu_id,
                      (unsigned long long)vm::entry_level((u32)cpu_id),
                      vm::monitor_ready((u32)cpu_id) ? 1u : 0u);
    ANKO_LOGF_VERBOSE("kernel/smp", "cpu%u local GIC target=%x timer ready", (u32)cpu_id,
                      drivers::gic::cpu_target(cpu_id));
    sched::mark_cpu_online(static_cast<u32>(cpu_id));
    __atomic_store_n(&g_cpu_started[cpu_id], 1u, __ATOMIC_RELEASE);
    libk::signal_event(); // wake the primary waiting on this arrival
    while (!__atomic_load_n(&g_release_secondaries, __ATOMIC_ACQUIRE))
        asm volatile("wfe" ::: "memory");
    sched::enter_secondary(static_cast<u32>(cpu_id));
}

} // namespace smp

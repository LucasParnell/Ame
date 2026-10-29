// kernel/drivers/timer/generic_timer.cc — ARM Generic Timer (CNTP_EL0).
//
// Spec ref: §5.10 IRQ and Timer
//
// The generic timer is available on all ARMv8-A cores.
// No MMIO — controlled via system registers:
//   CNTFRQ_EL0   — counter frequency (set by firmware; read-only at EL1)
//   CNTP_CTL_EL0 — control: ENABLE | IMASK | ISTATUS
//   CNTP_TVAL_EL0 — timer value (counts down from set value)
//   CNTPCT_EL0  — current counter value (read-only)
//
// CPU-owned scheduling/IPC deadlines share the timer with 100 Hz maintenance.
// Deadline interrupts never accelerate log time or the frequency governor.

#include "anko/platform.h"
#include "anko/platform_clock.h"
#include "midix/abi/cpu_limits.h"
#ifdef ANKO_BOARD_HUAWEI_ANNE
#include "boot_diagnostics.h"
#endif
#include "anko/sched.h"
#include "anko/power.h"
#include "anko/ipc.h"
#include "anko/smmuv3.h"
#include "anko/vm.h"
#include "anko/timer.h"
#include "anko/interrupt_mask.h"
#include "libk/cpu.h"
#include "libk/counter_scale.h"
#include "libk/metric.h"

namespace drivers::gic {
void irq_enable(u32 irq_id);
}

namespace drivers::timer {

constexpr u32 CNTP_CTL_ENABLE = 1u << 0;
// ISTATUS (bit 2) is read-only: set when condition is met.

static u64 g_tick_period = 0; // counter ticks between interrupts
static u64 g_tick_count = 0;
static u64 g_cpu_ticks[MX_HOST_MAX_CPUS]{};
static u64 g_frequency;
static libk::CounterScale g_scale;
struct alignas(64) CpuTimer {
    u64 maintenance, programmed;
    u64 deadlines[static_cast<u32>(DeadlineSource::Count)];
};
static CpuTimer timers[MX_HOST_MAX_CPUS]{};

using libk::metric_inc;
using libk::metric_load;

static void rearm() {
    auto& timer = timers[sched::current_cpu_id()];
    const u64 ticks = libk::cpu::counter_ticks();
    const u64 now = g_scale.nanoseconds(ticks);
    u64 next = timer.maintenance;
    for (auto deadline : timer.deadlines) {
        if (!deadline)
            continue;
        // Only convert deadlines preceding maintenance. The delta is at most
        // 10 ms, so delta * 32-bit CNTFRQ cannot overflow. Round upward.
        const u64 delta = deadline > now ? deadline - now : 0;
        if (delta > 10000000)
            continue;
        const u64 product = delta * g_frequency;
        const u64 delay = product / 1000000000 + (product % 1000000000 != 0);
        const u64 candidate = ticks + (delay ? delay : 1);
        if (candidate < next)
            next = candidate;
    }
    if (timer.programmed > ticks && timer.programmed <= next)
        return;
    timer.programmed = next;
    asm volatile("msr CNTP_CVAL_EL0, %0" : : "r"(next) : "memory");
    const u64 ctl = CNTP_CTL_ENABLE;
    asm volatile("msr CNTP_CTL_EL0, %0" : : "r"(ctl) : "memory");
    asm volatile("isb" ::: "memory");
}

void init(u32 irq_id) {
    u64 freq = 0;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    freq = platform::counter_frequency(freq);
    if (freq == 0)
        freq = 100000000;
    g_frequency = freq;
    g_scale.configure(static_cast<u32>(freq));
    g_tick_period = freq / 100;
    if (g_tick_period == 0)
        g_tick_period = 1;
    drivers::gic::irq_enable(irq_id);
    timers[sched::current_cpu_id()].maintenance = libk::cpu::counter_ticks() + g_tick_period;
    rearm();
}

void init_cpu(u32 irq_id) {
    drivers::gic::irq_enable(irq_id);
    timers[sched::current_cpu_id()].maintenance = libk::cpu::counter_ticks() + g_tick_period;
    rearm();
}

void set_deadline(DeadlineSource source, u64 deadline_ns) {
    if (!g_frequency || static_cast<u32>(source) >= static_cast<u32>(DeadlineSource::Count))
        return;
    kernel::InterruptGuard irq;
    auto& value = timers[sched::current_cpu_id()].deadlines[static_cast<u32>(source)];
    if (value == deadline_ns)
        return;
    value = deadline_ns;
    rearm();
}

// Called from GIC handler when IRQ 30 fires.
void handle_tick() {
#ifdef ANKO_BOARD_HUAWEI_ANNE
    anne::boot_timer_tick();
#endif
    const u32 cpu = sched::current_cpu_id();
    metric_inc(g_cpu_ticks[cpu]);
    auto& timer = timers[cpu];
    timer.programmed = 0;
    const u64 ticks = libk::cpu::counter_ticks();
    const u64 now = g_scale.nanoseconds(ticks);
    const bool maintenance = ticks >= timer.maintenance;
    if (maintenance) {
        const u64 periods = (ticks - timer.maintenance) / g_tick_period + 1;
        timer.maintenance += periods * g_tick_period;
        if (!cpu)
            libk::metric_add(g_tick_count, periods);
    }
    for (auto& deadline : timer.deadlines)
        if (deadline && deadline <= now)
            deadline = 0;
    // Phase C: poll the SMMUv3 event queue.  IOMMU faults are security events
    // and must not wait for a routed SMMU interrupt (routing one to a fault
    // handler is Phase D work); the poll is throttled to one tick in four and
    // bounded internally, and is a no-op when no SMMU is up.
    if (maintenance && iommu::smmuv3_ready() && (metric_load(g_tick_count) & 3ull) == 0) {
        (void)iommu::smmuv3_drain_events();
    }
    ipc::poll_deadline_waiters();
    vm::poll_blocked_vcpu_timers();
    sched::tick();
    if (maintenance && !cpu)
        power::frequency_tick(sched::sched_clock_ns(), sched::online_cpu_mask());
    rearm();
}

u64 tick_count() {
    return metric_load(g_tick_count);
}
u64 cpu_tick_count(u32 cpu) {
    return cpu < MX_HOST_MAX_CPUS ? metric_load(g_cpu_ticks[cpu]) : 0;
}

} // namespace drivers::timer

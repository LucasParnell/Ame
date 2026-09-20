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
// V0: fires every ~10ms (CNTFRQ / 100 ticks).
// On expiry, calls sched::tick() via GIC IRQ 30 → drivers::gic::handle_irq.

#include "anko/platform.h"
#include "anko/sched.h"
#include "anko/smmuv3.h"
#include "anko/vm.h"
#include "libk/metric.h"

namespace drivers::gic { void irq_enable(u32 irq_id); }

namespace drivers::timer {

constexpr u32 CNTP_CTL_ENABLE  = 1u << 0;
// ISTATUS (bit 2) is read-only: set when condition is met.

static u64 g_tick_period = 0; // counter ticks between interrupts
static u64 g_tick_count = 0;

using libk::metric_inc;
using libk::metric_load;

static void rearm() {
    asm volatile("msr CNTP_TVAL_EL0, %0" : : "r"(g_tick_period) : "memory");
    const u64 ctl = CNTP_CTL_ENABLE;
    asm volatile("msr CNTP_CTL_EL0, %0" : : "r"(ctl) : "memory");
    asm volatile("isb" ::: "memory");
}

void init(u32 irq_id) {
    u64 freq = 0;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    if (freq == 0) freq = 100000000;
    g_tick_period = freq / 100;
    if (g_tick_period == 0) g_tick_period = 1;
    drivers::gic::irq_enable(irq_id);
    rearm();
}

void init_cpu(u32 irq_id) {
    drivers::gic::irq_enable(irq_id);
    rearm();
}

// Called from GIC handler when IRQ 30 fires.
void handle_tick() {
    metric_inc(g_tick_count);
    rearm();
    // Phase C: poll the SMMUv3 event queue.  IOMMU faults are security events
    // and must not wait for a routed SMMU interrupt (routing one to a fault
    // handler is Phase D work); the poll is throttled to one tick in four and
    // bounded internally, and is a no-op when no SMMU is up.
    if (iommu::smmuv3_ready() && (metric_load(g_tick_count) & 3ull) == 0) {
        (void)iommu::smmuv3_drain_events();
    }
    vm::poll_blocked_vcpu_timers();
    sched::tick();
}

u64 tick_count() {
    return metric_load(g_tick_count);
}

} // namespace drivers::timer

#include "anko/timer.h"
#include "anko/sched.h"
#include "libk/counter_scale.h"
#include <cassert>
#include <cstdint>

u64 audit_timer_ticks = 1000, audit_programmed = 0;
u32 audit_frequency = 1920000, audit_timer_cpu = 0;
unsigned audit_timer_writes, audit_scheduler_ticks, audit_ipc_polls, audit_power_ticks;
u64 audit_ticks() {
    return audit_timer_ticks;
}
namespace sched {
u32 current_cpu_id() {
    return audit_timer_cpu;
}
u64 online_cpu_mask() {
    return 3;
}
u64 sched_clock_ns() {
    libk::CounterScale scale;
    scale.configure(audit_frequency);
    return scale.nanoseconds(audit_ticks());
}
void tick() {
    ++audit_scheduler_ticks;
}
} // namespace sched
namespace drivers::gic {
void irq_enable(u32) {
}
} // namespace drivers::gic
namespace ipc {
void poll_deadline_waiters() {
    ++audit_ipc_polls;
}
} // namespace ipc
namespace power {
void frequency_tick(u64, u64) {
    ++audit_power_ticks;
}
} // namespace power
namespace vm {
void poll_blocked_vcpu_timers() {
}
} // namespace vm
namespace iommu {
bool smmuv3_ready() {
    return false;
}
u32 smmuv3_drain_events() {
    return 0;
}
} // namespace iommu
namespace drivers::timer {
void init(u32);
void init_cpu(u32);
void handle_tick();
u64 tick_count();
u64 cpu_tick_count(u32);
} // namespace drivers::timer
int main() {
    using namespace drivers::timer;
    init(30);
    const auto maintenance = audit_programmed;
    assert(maintenance == audit_ticks() + audit_frequency / 100);
    const auto now = sched::sched_clock_ns();
    set_deadline(DeadlineSource::Scheduler, now + 500000);
    assert(audit_programmed >= audit_ticks() + 959 && audit_programmed <= audit_ticks() + 961);
    set_deadline(DeadlineSource::Ipc, now + 200000);
    assert(audit_programmed >= audit_ticks() + 383 && audit_programmed <= audit_ticks() + 385);
    const auto early = audit_programmed;
    set_deadline(DeadlineSource::Ipc, 0);
    assert(audit_programmed == early); // harmless early IRQ; no extra MMIO write
    audit_timer_ticks = early;
    handle_tick();
    assert(cpu_tick_count(0) == 1 && tick_count() == 0 && audit_power_ticks == 0 &&
           audit_ipc_polls == 1);
    assert(audit_programmed > audit_ticks() && audit_programmed < maintenance);
    audit_timer_ticks = audit_programmed;
    handle_tick();
    assert(tick_count() == 0 && audit_power_ticks == 0);
    assert(audit_programmed == maintenance);
    audit_timer_ticks = maintenance;
    handle_tick();
    assert(tick_count() == 1 && audit_power_ticks == 1 && audit_scheduler_ticks == 3);
    // Late maintenance advances elapsed periods without drifting from IRQ time.
    audit_timer_ticks += 3 * audit_frequency / 100;
    handle_tick();
    assert(tick_count() == 4 && audit_power_ticks == 2);
    const auto cpu0_next = audit_programmed;
    audit_timer_cpu = 1;
    init_cpu(30);
    set_deadline(DeadlineSource::Scheduler, sched::sched_clock_ns() + 100000);
    assert(audit_programmed < cpu0_next);
    audit_timer_ticks = audit_programmed;
    handle_tick();
    assert(cpu_tick_count(1) == 1 && tick_count() == 4 && audit_power_ticks == 2);
}

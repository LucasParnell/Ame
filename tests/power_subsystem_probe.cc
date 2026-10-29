#include "anko/power.h"
#include "libk/idle_account.h"
#include "libk/integer.h"
#include "../kernel/power/anne/thermal.h"
#include "../kernel/power/anne/battery.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include <thread>
#include <atomic>
static u64 now = 1000000000;
namespace sched {
u64 sched_clock_ns() {
    return now;
}
u64 online_cpu_mask() {
    return 3;
}
} // namespace sched
static u32 applied;
static bool refuse;
static SysError set_frequency(u32 domain, u32 khz) {
    assert(domain == 0);
    if (refuse)
        return E_BUSY;
    applied = khz;
    return E_OK;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    alignas(4) unsigned char pmic[4096]{};
    const libk::Mmio registers{pmic, sizeof(pmic)};
    registers.write8(0x004 * 4, 1);
    std::int32_t current = 0;
    registers.write8(0x1f2 * 4, 10661 & 255);
    registers.write8(0x1f3 * 4, 10661 >> 8);
    assert(anne::battery::current_ua(registers, current) && current == 5000000);
    registers.write8(0x1f2 * 4, (65536 - 10661) & 255);
    registers.write8(0x1f3 * 4, (65536 - 10661) >> 8);
    assert(anne::battery::current_ua(registers, current) && current == -5000000);
    registers.write8(0x004 * 4, 2);
    assert(!anne::battery::current_ua(registers, current));
    std::int32_t temperature = 0;
    assert(anne::thermal::millicelsius(116, 116, 922, temperature) && temperature == -40000);
    assert(anne::thermal::millicelsius(922, 116, 922, temperature) && temperature == 125000);
    assert(anne::thermal::millicelsius(499, 116, 922, temperature) && temperature == 38405);
    assert(!anne::thermal::millicelsius(~u64{0}, 116, 922, temperature));
    assert(!anne::thermal::millicelsius(116, 116, 116, temperature));
    assert(libk::ratio_per_mille(~u64{0}, ~u64{0}) == 1000);
    assert(libk::ratio_per_mille((~u64{0}) / 2, ~u64{0}) == 499);
    assert(libk::ratio_per_mille(1, 0) == 0);
    libk::IdleAccount activity;
    activity.enter(100);
    std::uint64_t idle, entries;
    assert(activity.read(150, idle, entries) && idle == 50 && entries == 1);
    activity.leave(200);
    assert(activity.read(400, idle, entries) && idle == 100 && entries == 1);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            activity.enter(1000 + i * 2);
            activity.leave(1001 + i * 2);
        }
        done = true;
    });
    while (!done)
        if (activity.read(100000, idle, entries))
            assert(idle <= 100100);
    writer.join();
    assert(activity.read(100000, idle, entries) && idle == 10100 && entries == 10001);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> dtb((std::istreambuf_iterator<char>(input)), {});
    smp::CpuTopology topology{};
    topology.count = 2;
    topology.mpidr[1] = 1;
    power::activity_init();
    power::frequency_init(reinterpret_cast<PhysAddr>(dtb.data()), topology);
    power::thermal_init(reinterpret_cast<PhysAddr>(dtb.data()));
    MxPowerSnapshot sample{};
    power::snapshot(sample);
    assert(sample.domain_count == 1 && sample.domains[0].cpu_mask == 3);
    assert(sample.domains[0].opp_count == 3 && !(sample.domains[0].flags & MX_FREQ_LOAD));
    assert(power::frequency_request(0, 1000) == E_NOTIMPL);
    assert(power::frequency_limits(4, 1, 2) == E_INVALID);
    assert(power::frequency_limits(0, 1500000, 1600000) == E_INVALID);
    assert(power::frequency_limits(0, 400000, 2000000) == E_OK);
    power::thermal_publish(0, 40000, true, now);
    power::thermal_publish(2, 40000, true, now);
    power::frequency_backend(set_frequency);
    assert(power::frequency_request(0, 900000) == E_OK && applied == 1000000);
    assert(power::frequency_thermal_cap(0, 600000) == E_OK && applied == 400000);
    assert(power::frequency_request(0, 2000000) == E_OK && applied == 400000);
    assert(power::frequency_thermal_cap(0, 1) == E_INVALID);
    refuse = true;
    assert(power::frequency_thermal_cap(0, 1000000) == E_OK);
    assert(power::frequency_request(0, 1000000) == E_BUSY);
    refuse = false;
    assert(power::frequency_thermal_cap(0, 0) == E_OK);
    power::idle_enter(0, now);
    power::idle_enter(1, now);
    now += 100000000;
    power::frequency_tick(now, 3);
    power::snapshot(sample);
    assert(sample.domains[0].flags & MX_FREQ_LOAD);
    assert(sample.domains[0].busy_permille == 0 && sample.domains[0].target_khz == 400000);
    assert(sample.idle_valid_mask == 3 && sample.cpus[0].idle_ns == 100000000);
    power::idle_leave(0, now);
    power::idle_leave(1, now);
    now += 100000000;
    power::frequency_tick(now, 3);
    power::snapshot(sample);
    assert(sample.domains[0].busy_permille == 1000 && sample.domains[0].target_khz == 2000000);
    power::thermal_publish(0, 80000, true, now);
    now += 100000000;
    power::frequency_tick(now, 3);
    power::snapshot(sample);
    assert(sample.domains[0].thermal_cap_khz == 400000 && applied == 400000);
    power::thermal_publish(0, 40000, true, now);
    now += 100000000;
    power::frequency_tick(now, 3);
    now += 20000000;
    power::frequency_tick(now, 3);
    power::snapshot(sample);
    assert(!sample.domains[0].thermal_cap_khz && applied == 2000000);
    assert(power::charger_publish(0x20b2) == E_OK);
    assert(power::charger_publish(1ull << 32) == E_INVALID);
    assert(power::thermal_publish(0, -5000, true, now) == E_OK);
    assert(power::thermal_publish(0, 150001, true, now) == E_INVALID);
    assert(power::thermal_publish(4, 1, true, now) == E_INVALID);
    power::snapshot(sample);
    assert(sample.flags & MX_POWER_CHARGER_VALID);
    assert(sample.charger_flags ==
           (MX_CHARGER_INPUT | MX_CHARGER_ENABLED | MX_CHARGER_DONE | MX_CHARGER_THERMAL));
    assert(sample.thermals[0].valid && sample.thermals[0].millicelsius == -5000);
    now += 4000000000;
    power::snapshot(sample);
    assert(!(sample.flags & MX_POWER_CHARGER_VALID) && !sample.thermals[0].valid);
    assert(power::charger_publish(0) == E_OK);
    power::charger_invalidate();
    power::snapshot(sample);
    assert(!(sample.flags & MX_POWER_CHARGER_VALID));
    dtb[0] = 0;
    power::frequency_init(reinterpret_cast<PhysAddr>(dtb.data()), topology);
    power::snapshot(sample);
    assert(!sample.domain_count);
}

#include "anko/power.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
namespace power {
bool thermal_snapshot(u64, MxPowerThermal*) {
    return false;
}
} // namespace power
namespace sched {
u64 sched_clock_ns() {
    return 1000000000;
}
u64 online_cpu_mask() {
    return 255;
}
} // namespace sched
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> dtb((std::istreambuf_iterator<char>(input)), {});
    const auto address = reinterpret_cast<PhysAddr>(dtb.data());
    const auto topology = smp::read_cpu_topology(address, 0x102);
    assert(topology.count == 8 && topology.mpidr[0] == 0x102);
    power::frequency_init(address, topology);
    MxPowerDomain domains[MX_POWER_DOMAINS]{};
    u32 count = 0;
    power::frequency_snapshot(1000000000, 255, domains, count);
    assert(count == 2);
    assert(domains[0].cpu_mask == 0x1e && domains[1].cpu_mask == 0xe1);
    assert(domains[0].opp_count == 4 && domains[1].opp_count == 5);
    assert(domains[0].min_khz == 480000 && domains[0].max_khz == 1709000);
    assert(domains[1].min_khz == 1402000 && domains[1].max_khz == 2362000);
    std::puts("Anne DT: eight remapped CPUs, two clock domains, all nine OPPs validated");
}

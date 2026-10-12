#pragma once
#include "anko/types.h"
#include "midix/abi/cpu_limits.h"
namespace smp {
constexpr u64 kMpidrAffinityMask = 0xff00ffffffull;
struct CpuTopology {
    u32 count;
    u64 mpidr[MX_HOST_MAX_CPUS];
    bool psci_available;
    bool psci_hvc;
    u64 cpu_on;
};
CpuTopology read_cpu_topology(PhysAddr dtb, u64 boot_mpidr);
}

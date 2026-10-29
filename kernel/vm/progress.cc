#include "anko/vm_progress.h"
#include "anko/sched.h"
#include "anko/spinlock.h"

namespace vm {
namespace { ProgressRecord records[MX_HOST_MAX_CPUS]{}; }
void note_progress(u64 stage, u64 a, u64 b) {
    kernel::InterruptGuard guard;
    const u32 cpu = sched::current_cpu_id();
    if (cpu < MX_HOST_MAX_CPUS) records[cpu].publish(stage, sched::sched_clock_ns(), a, b);
}
bool read_progress(u32 cpu, MxDiagVmProgress& out) {
    return cpu < MX_HOST_MAX_CPUS && records[cpu].snapshot(cpu, out);
}
} // namespace vm

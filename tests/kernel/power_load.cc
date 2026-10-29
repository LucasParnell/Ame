// Ephemeral native workload: exercise both DVFS domains through scheduler load.
// Run through the existing USB app container; it never writes phone storage.
#include "mochi/boot_log.h"
#include "midix/abi/diagnostics.h"
#include "libk/printf.h"
namespace {
mx_u64_t deadline, finished;
struct alignas(64) Result {
    mx_u64_t checksum;
} results[MX_POWER_CPUS];
void worker(mx_u64_t cpu) {
    mx_u64_t value = cpu + 1;
    while (mx_clock_ns() < deadline) {
        for (unsigned i = 0; i < 4096; ++i) {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
        }
    }
    results[cpu].checksum = value;
    __atomic_fetch_or(&finished, mx_u64_t{1} << cpu, __ATOMIC_RELEASE);
    sys_thread_exit(0);
    for (;;)
        sys_yield();
}
void marker(const char* line) {
    sys_debug_puts(line, libk::str::length(line));
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    mochi::boot::ready(boot);
    MxPowerSnapshot before{};
    if (sys_debug_read(MX_DIAG_POWER, 0, &before, sizeof(before)) != sizeof(before))
        mx_exit(1);
    mx_handle_t threads[MX_POWER_CPUS]{};
    deadline = mx_clock_ns() + 2000000000;
    for (unsigned cpu = 0; cpu < MX_POWER_CPUS; ++cpu)
        if (before.online_mask & (mx_u64_t{1} << cpu)) {
            const auto thread =
                sys_thread_create(0, reinterpret_cast<mx_u64_t>(&worker), cpu, MX_THREAD_SUSPENDED);
            if (thread <= 0)
                mx_exit(2);
            threads[cpu] = static_cast<mx_handle_t>(thread);
            if (sys_thread_set_affinity(threads[cpu], mx_u64_t{1} << cpu) ||
                sys_thread_resume(threads[cpu]))
                mx_exit(3);
        }
    mx_u32_t peak[MX_POWER_DOMAINS]{};
    MxPowerSnapshot sample{};
    while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) != before.online_mask) {
        if (mx_clock_ns() > deadline + 2000000000)
            mx_exit(4);
        if (sys_debug_read(MX_DIAG_POWER, 0, &sample, sizeof(sample)) != sizeof(sample))
            mx_exit(5);
        for (unsigned d = 0; d < sample.domain_count; ++d)
            if ((sample.domains[d].flags & MX_FREQ_OBSERVED) &&
                sample.domains[d].observed_khz > peak[d])
                peak[d] = sample.domains[d].observed_khz;
        mx_yield();
    }
    bool valid = sample.domain_count == 2;
    for (unsigned d = 0; d < sample.domain_count; ++d) {
        char line[160];
        const auto& domain = sample.domains[d];
        libk::snprintf(line, sizeof(line),
                       "POWER_LOAD domain=%u peak_khz=%u max_khz=%u transitions=%u failures=%u\n",
                       d, peak[d], domain.max_khz, domain.transitions, domain.failures);
        marker(line);
        valid &= peak[d] == domain.max_khz && !domain.failures;
    }
    for (auto thread : threads)
        if (thread)
            (void)sys_cap_revoke(thread);
    mx_exit(valid ? 0 : 6);
    for (;;)
        mx_yield();
}

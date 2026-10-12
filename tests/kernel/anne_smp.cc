// Bounded Ressha workload: one pinned worker on every advertised online CPU.
#include "mochi/boot.h"
#include "mochi/syscall.h"
#include "mochi/log.h"
#include "libk/str.h"
#include "midix/abi/cpu_limits.h"
#include "dango/vm.h"

namespace {
uint64_t online = 0;
uint64_t arrived = 0;
uint64_t completed = 0;
uint32_t failed = 0;
CapHandle completion = 0;
void marker(const char* text) { sys_debug_puts(text, libk::str::length(text)); }
volatile uint64_t results[MX_HOST_MAX_CPUS]{};
[[noreturn]] void fail(const char* reason) {
    marker(reason);
    sys_thread_exit(1);
    for (;;) sys_yield();
}
void worker(uint64_t expected) {
    const int64_t observed = sys_thread_get_cpu();
    if (observed != static_cast<int64_t>(expected)) __atomic_store_n(&failed, 1u, __ATOMIC_RELEASE);
    __atomic_fetch_or(&arrived, uint64_t{1} << expected, __ATOMIC_RELEASE);
    const uint64_t limit = mx_clock_ns() + 3000000000ull;
    while (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != online && mx_clock_ns() < limit) sys_yield();
    if (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) != online) __atomic_store_n(&failed, 1u, __ATOMIC_RELEASE);
    const uint64_t until = mx_clock_ns() + 200000000ull;
    uint64_t sum = expected + 1, rounds = 0;
    do {
        for (uint32_t i = 0; i < 32768; ++i) sum = sum * 6364136223846793005ull + i + 1;
        results[expected] = sum; ++rounds;
    } while (mx_clock_ns() < until);
    char storage[128]; libk::str::Buffer line(storage);
    line.format("SMP_WORK cpu=%llu observed=%lld rounds=%llu\n", (unsigned long long)expected,
                (long long)observed, (unsigned long long)rounds);
    marker(line.data);
    __atomic_fetch_or(&completed, uint64_t{1} << expected, __ATOMIC_RELEASE);
    sys_notify(completion);
    sys_thread_exit(0);
    for (;;) sys_yield();
}
}
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    const auto* mask = mochi::boot::find(boot, "cpu.mask");
    const auto* count = mochi::boot::find(boot, "cpu.count");
    if (!mask || !count || count->aux0 != 8 || mask->aux0 != 0xff) fail("SMP_WORK expected eight CPUs\n");
    online = mask->aux0;
    const auto db = sys_doorbell_create();
    if (db <= 0) fail("SMP_WORK doorbell create failed\n");
    completion = static_cast<CapHandle>(db);
    CapHandle threads[MX_HOST_MAX_CPUS]{};
    for (uint32_t cpu = 0; cpu < MX_HOST_MAX_CPUS; ++cpu) {
        if (!(online & (uint64_t{1} << cpu))) continue;
        const auto t = sys_thread_create(0, reinterpret_cast<uint64_t>(&worker), cpu, MX_THREAD_SUSPENDED);
        if (t <= 0) fail("SMP_WORK thread create failed\n");
        threads[cpu] = static_cast<CapHandle>(t);
        // Init polls readiness in the latency band. Keep the validation workers
        // in that same band so readiness polling cannot starve the CPU0 worker.
        dango::SchedContext context{};
        context.band = dango::SCHED_BAND_LATENCY;
        context.weight = 1024;
        context.core_mask = uint64_t{1} << cpu;
        context.home_cpu = cpu;
        const auto sched = sys_sched_context_create(&context, sizeof(context));
        if (sched <= 0 || sys_sched_context_bind_thread(threads[cpu], static_cast<CapHandle>(sched)))
            fail("SMP_WORK scheduler context failed\n");
        if (sys_thread_set_affinity(threads[cpu], uint64_t{1} << cpu)) fail("SMP_WORK affinity failed\n");
    }
    for (const auto t : threads) if (t && sys_thread_resume(t)) fail("SMP_WORK resume failed\n");
    while (true) {
        const auto expected = sys_doorbell_read(completion);
        if (__atomic_load_n(&completed, __ATOMIC_ACQUIRE) == online) break;
        if (sys_wait_doorbell(completion, expected)) fail("SMP_WORK completion wait failed\n");
    }
    if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) fail("SMP_WORK wrong CPU or barrier timeout\n");
    marker("SMP_WORK complete mask=ff workers=8\n");
    mochi::boot::ready(boot);
    // Remain blocked after readiness; this validation service consumes no idle CPU.
    for (;;) {
        const auto expected = sys_doorbell_read(completion);
        sys_wait_doorbell(completion, expected);
    }
}

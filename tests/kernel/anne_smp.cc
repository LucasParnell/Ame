// Bounded Ressha workload: one pinned worker on every advertised online CPU.
#include "mochi/boot.h"
#include "mochi/syscall.h"
#include "mochi/log.h"
#include "libk/str.h"
#include "midix/abi/cpu_limits.h"
#include "midix/abi/diagnostics.h"
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
void check_diagnostics() {
    MxDiagSystem info{};
    if (sys_debug_read(MX_DIAG_SYSTEM, 0, &info, sizeof(info)) != sizeof(info) ||
        info.version != MX_DIAG_VERSION || info.online_mask != online ||
        info.cpu_limit != MX_HOST_MAX_CPUS || info.current_el != 1 ||
        !info.boot_log_size || info.boot_log_size > info.boot_log_capacity)
        fail("DIAG_READ system failed\n");
    for (uint32_t cpu = 0; cpu < info.cpu_limit; ++cpu) {
        MxDiagCpu record{};
        if (sys_debug_read(MX_DIAG_CPU, cpu, &record, sizeof(record)) != sizeof(record) ||
            record.version != MX_DIAG_VERSION || record.cpu != cpu || !record.online ||
            (record.boot_entry_el != 1 && record.boot_entry_el != 2) ||
            record.monitor_ready != (record.boot_entry_el == 2))
            fail("DIAG_READ CPU failed\n");
    }
    char first[64]{}, second[64]{};
    if (sys_debug_read(MX_DIAG_BOOT_LOG, 0, first, sizeof(first)) != sizeof(first) ||
        sys_debug_read(MX_DIAG_BOOT_LOG, 0, second, sizeof(second)) != sizeof(second))
        fail("DIAG_READ retained prefix failed\n");
    for (uint32_t i = 0; i < sizeof(first); ++i)
        if (first[i] != second[i]) fail("DIAG_READ prefix changed\n");
    if (sys_debug_read(MX_DIAG_BOOT_LOG, ~uint64_t{0}, first, sizeof(first)) != 0 ||
        sys_debug_read(MX_DIAG_SYSTEM, 0, &info, sizeof(info) - 1) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_SYSTEM, 1, &info, sizeof(info)) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_CPU, MX_HOST_MAX_CPUS, &info, sizeof(info)) != MX_ERR_INVALID ||
        sys_debug_read(99, 0, &info, sizeof(info)) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_BOOT_LOG, 0, first, MX_DIAG_READ_MAX + 1) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_BOOT_LOG, 0, first, 0) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_SYSTEM, 0, nullptr, sizeof(info)) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_SYSTEM, 0, reinterpret_cast<void*>(0x1000), sizeof(info)) != MX_ERR_FAULT)
        fail("DIAG_READ request validation failed\n");
    marker("DIAG_READ complete records=8 retained_repeatable=1 bounds_checked=1\n");
    // This workload runs against the synthetic Huawei board. It must never
    // access the phone's physical RPMB window or issue its vendor SMCs.
    MxDiagEl3 el3{};
    if (sys_debug_read(MX_DIAG_EL3, 0, &el3, sizeof(el3)) != sizeof(el3) ||
        el3.available != 0 || el3.request_address != 0)
        fail("DIAG_READ synthetic EL3 metadata failed\n");
    for (uint64_t index = 1; index <= MX_DIAG_EL3_MAX_INDEX; ++index)
        if (sys_debug_read(MX_DIAG_EL3, index, &el3, sizeof(el3)) != MX_ERR_NOT_IMPLEMENTED)
            fail("DIAG_READ synthetic EL3 request executed\n");
    if (sys_debug_read(MX_DIAG_EL3, MX_DIAG_EL3_MAX_INDEX + 1, &el3, sizeof(el3)) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_EL3, ~uint64_t{0}, &el3, sizeof(el3)) != MX_ERR_INVALID ||
        sys_debug_read(MX_DIAG_EL3, 0, reinterpret_cast<void*>(0x1000), sizeof(el3)) != MX_ERR_FAULT)
        fail("DIAG_READ EL3 bounds failed\n");
    char storage[96]; libk::str::Buffer line(storage);
    line.format("DIAG_READ synthetic_el3_unavailable=1 native_requests_blocked=%llu\n",
                static_cast<unsigned long long>(MX_DIAG_EL3_MAX_INDEX));
    marker(line.data);
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
    check_diagnostics();
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

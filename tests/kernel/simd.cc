// Scalar harness deliberately keeps test patterns live in all SIMD registers
// across real syscalls, timer preemption, blocking and cross-CPU dispatch.
#include "mochi/boot.h"
#include "dango/vm.h"
#include "midix/abi/diagnostics.h"
#include "libk/str.h"

namespace {
struct alignas(16) State { uint64_t v[32][2]; uint32_t fpsr, fpcr; };
extern "C" void test_simd_seed(uint64_t seed, uint64_t fpcr, uint64_t fpsr);
extern "C" void test_simd_capture(State* state);
CapHandle completion, park, migrate_gate;
CapHandle contexts[8];
CapHandle worker_park[10];
uint32_t cpu_count, completed, phase, target_cpu;
uint64_t witness[10];
[[noreturn]] void fail(const char* message) {
    sys_debug_puts("TEST FAILED: SIMD ", 18);
    sys_debug_puts(message, libk::str::length(message));
    sys_debug_puts("\n", 1);
    for (;;) sys_yield();
}
void check(bool condition, const char* message) { if (!condition) fail(message); }
void zero_state() {
    State state; test_simd_capture(&state);
    for (const auto& reg : state.v)
        check(!reg[0] && !reg[1], "new thread inherited vector data");
    check(!state.fpcr && !state.fpsr, "new thread inherited control/status");
}
void verify(uint64_t seed, uint32_t fpcr, uint32_t fpsr) {
    State state; test_simd_capture(&state);
    for (uint32_t i = 0; i < 32; ++i) {
        const uint64_t expected = ((seed + i) & 255) * 0x0101010101010101ull;
        check(state.v[i][0] == expected && state.v[i][1] == expected, "vector register corruption");
    }
    check(state.fpcr == fpcr && state.fpsr == fpsr, "FPCR/FPSR corruption");
}
void worker(uint64_t id) {
    zero_state();
    const auto seed = 1 + id * 19;
    const uint32_t fpcr = (id % 4) << 22, fpsr = (1u << (id % 5)) | (1u << 27);
    test_simd_seed(seed, fpcr, fpsr);
    for (uint32_t round = 0; round < 64; ++round) {
        verify(seed, fpcr, fpsr);
        sys_yield();
        verify(seed, fpcr, fpsr);
        // No syscalls inside this loop: IRQ preemption must preserve the live
        // vector state too, even in the middle of a long EL0 computation.
        uint64_t sum = witness[id] + id + 1;
        for (uint32_t i = 0; i < 100000; ++i) sum = sum * 6364136223846793005ull + i;
        witness[id] = sum;
        verify(seed, fpcr, fpsr);
        if (!(round % 8)) {
            const auto expected = sys_doorbell_read(worker_park[id]);
            check(mx_doorbell_wait_until(worker_park[id], expected, mx_clock_ns() + 200000) == MX_OK,
                  "timed blocking wait");
            verify(seed, fpcr, fpsr);
        }
    }
    __atomic_fetch_add(&completed, 1u, __ATOMIC_RELEASE);
    sys_notify(completion);
    sys_thread_exit(0);
    for (;;) sys_yield();
}
void migrator(uint64_t) {
    zero_state();
    test_simd_seed(201, 2u << 22, 0x15);
    for (uint32_t turn = 0; turn < cpu_count * 2; ++turn) {
        __atomic_store_n(&phase, turn + 1, __ATOMIC_RELEASE);
        sys_notify(completion);
        while (sys_doorbell_read(migrate_gate) == turn)
            check(!sys_wait_doorbell(migrate_gate, turn), "migration wait");
        check(sys_thread_get_cpu() == __atomic_load_n(&target_cpu, __ATOMIC_ACQUIRE),
              "migration did not reach requested CPU");
        verify(201, 2u << 22, 0x15);
    }
    __atomic_store_n(&phase, cpu_count * 2 + 1, __ATOMIC_RELEASE);
    sys_notify(completion);
    sys_thread_exit(0);
    for (;;) sys_yield();
}
CapHandle create(void (*entry)(uint64_t), uint64_t id, uint32_t cpu) {
    const auto thread = sys_thread_create(0, reinterpret_cast<uint64_t>(entry), id, MX_THREAD_SUSPENDED);
    check(thread > 0, "thread create");
    check(!sys_sched_context_bind_thread(thread, contexts[cpu]), "initial context binding");
    check(!sys_thread_resume(thread), "thread resume");
    return thread;
}
}
extern "C" [[noreturn]] void _start() {
    zero_state();
    const auto* boot = mochi::boot::map();
    const auto* count = mochi::boot::find(boot, "cpu.count");
    check(count && count->aux0 && count->aux0 <= 8, "CPU count");
    cpu_count = count->aux0;
    completion = sys_doorbell_create(); park = sys_doorbell_create(); migrate_gate = sys_doorbell_create();
    check(completion && park && migrate_gate, "doorbell create");
    for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
        dango::SchedContext context{};
        context.band = dango::SCHED_BAND_LATENCY; context.weight = 1024;
        context.core_mask = uint64_t{1} << cpu; context.home_cpu = cpu;
        const auto cap = sys_sched_context_create(&context, sizeof(context));
        check(cap > 0, "context create"); contexts[cpu] = cap;
    }
    // At least two contenders on CPU0, plus a pattern holder on every CPU.
    for (uint32_t id = 0; id < cpu_count + 1; ++id) {
        const auto bell = sys_doorbell_create();
        check(bell > 0, "worker doorbell"); worker_park[id] = bell;
        create(worker, id, id % cpu_count);
    }
    for (;;) {
        const auto expected = sys_doorbell_read(completion);
        if (__atomic_load_n(&completed, __ATOMIC_ACQUIRE) == cpu_count + 1) break;
        check(!sys_wait_doorbell(completion, expected), "completion wait");
    }
    // A new thread created after the pattern holders still starts clean.
    const auto thread = create(migrator, 0, 0);
    for (uint32_t turn = 0; turn < cpu_count * 2; ++turn) {
        while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != turn + 1) sys_yield();
        const auto destination = (turn + 1) % cpu_count;
        const auto deadline = mx_clock_ns() + 2000000000ull;
        mx_status_t rc;
        // Binding is allowed only after the worker has saved its frame/state
        // and released execution ownership on its old CPU.
        do {
            rc = sys_sched_context_bind_thread(thread, contexts[destination]);
            if (rc == MX_OK) break;
            check(rc == MX_ERR_BUSY && mx_clock_ns() < deadline, "migration binding");
            sys_yield();
        } while (true);
        __atomic_store_n(&target_cpu, destination, __ATOMIC_RELEASE);
        check(!sys_notify(migrate_gate), "migration notify");
    }
    while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != cpu_count * 2 + 1) sys_yield();
    uint64_t preempts = 0;
    for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
        MxDiagCpu info{};
        check(sys_debug_read(MX_DIAG_CPU, cpu, &info, sizeof(info)) == sizeof(info), "CPU diagnostics");
        preempts += info.timer_preempts;
    }
    check(preempts != 0, "workload did not exercise timer preemption");
    const char marker[] = "userland SIMD regression passed\n";
    sys_debug_puts(marker, sizeof(marker) - 1);
    for (;;) sys_wait_doorbell(park, sys_doorbell_read(park));
}

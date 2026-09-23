// tests/kernel/restart.cc — supervision restart regression.
//
// Runs as the bundle's `init` (like tests/kernel/lifecycle.cc): it holds the
// root supervisor capability and drives real child processes, exercising the
// steady-state contract userland/init relies on:
//
//   * generation-tagged exit records are drained and matched to the exact
//     incarnation that died, never to a same-named later one;
//   * restarting a dependency stops its dependents and relaunches them as new
//     generations;
//   * a deliberate stop never accrues crash backoff, while repeated crashes do.
//
// The restart/backoff/dependency/matching decisions come from the same
// userland/init/supervision.h the supervisor uses, so this asserts the shared
// policy rather than a private copy of it.
//
// Success marker: "supervision regression passed".

#include "mochi/boot.h"
#include "midix/format/bundle.h"
#include "midix/abi/image.h"
#include "libk/bytes.h"
#include "libk/str.h"
#include "supervision.h"

using supervision::backoff_ns;
using supervision::dependent_closure;
using supervision::kFailureCap;
using supervision::match_identity;
using supervision::restart_decision;

constexpr uint32_t kMaxServices = 4;

static CapHandle supervisor;
static uint32_t cpu_count;
static midix::format::Bundle bundle;

static void print(const char* text) {
    sys_debug_puts(text, libk::str::length(text));
}
[[noreturn]] static void fail(const char* message) {
    print("TEST FAILED: ");
    print(message);
    print("\n");
    while (true) {
        sys_yield();
    }
}
static void check(bool value, const char* message) {
    if (!value) {
        fail(message);
    }
}

// ---------------------------------------------------------------------------
// Child processes (all built from the same test.busy image).
// ---------------------------------------------------------------------------
static const uint8_t* object_image(const char* name, uint64_t* size) {
    for (uint32_t i = 1; i < bundle.header.object_count; ++i) {
        MxBundleObject object{};
        const char* object_name = nullptr;
        check(midix::format::object(bundle, i, object, object_name), "test object lookup");
        if (mochi::boot::streq(object_name, name)) {
            *size = object.size;
            return bundle.data + object.offset;
        }
    }
    return nullptr;
}

static void spawn_child(CapHandle* process, CapHandle* thread, uint64_t* identity,
                        const char* name) {
    uint64_t size = 0;
    const uint8_t* data = object_image(name, &size);
    check(data && size, "test image absent");
    static const char process_name[32] = "test.child";
    auto result = sys_spawn(data, size, MX_POLICY_PRINCIPAL_NATIVE_SANDBOXED, supervisor,
                            MX_SPAWN_SUSPENDED, process_name);
    check(result.value > 0 && result.detail > 0, "spawn");
    *process = static_cast<CapHandle>(result.value);
    *thread = static_cast<CapHandle>(result.detail);
    MxCapInfo info{};
    check(!sys_cap_info(*process, &info, sizeof(info)), "process identity");
    *identity = info.base;
    check(!sys_thread_set_affinity(*thread, 1ull << (cpu_count - 1)), "remote affinity");
    check(!sys_thread_resume(*thread), "resume");
}

static bool poll_exit(uint64_t* identity, long* status) {
    auto event = sys_supervisor_poll(supervisor);
    if (event.value <= 0) return false;
    *identity = static_cast<uint64_t>(event.value);
    *status = event.detail;
    return true;
}

static void await_exit(uint64_t identity, long expected, const char* message) {
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline) {
        uint64_t got = 0;
        long status = 0;
        if (poll_exit(&got, &status)) {
            check(got == identity && status == expected, message);
            return;
        }
        sys_yield();
    }
    fail(message);
}

// ---------------------------------------------------------------------------
// 1. Generation matching: an exit record matches the incarnation that died, and
//    restarting produces a distinct generation even when a PID slot is reused.
// ---------------------------------------------------------------------------
static void test_generation_matching() {
    CapHandle process = kNullCap, thread = kNullCap;
    uint64_t first = 0;
    spawn_child(&process, &thread, &first, "test.busy");
    check(!sys_kill(process), "first kill");
    await_exit(first, -1, "first exit identity");
    check(sys_cap_identify(process) < 0 && sys_cap_identify(thread) < 0,
          "stale caps after exit");

    CapHandle restarted = kNullCap, restarted_thread = kNullCap;
    uint64_t second = 0;
    spawn_child(&restarted, &restarted_thread, &second, "test.busy");
    check(second != first, "restart is a new generation");
    check(!sys_kill(restarted), "restart kill");
    await_exit(second, -1, "restarted exit identity");
    print("generation-tagged exits: matched to the right incarnation\n");
}

static void test_generation_reuse() {
    // PID slots are reserved until their exit is acknowledged, so identities can
    // only stay unique across reuse because they carry the process generation.
    uint64_t previous[65]{};
    for (uint32_t cycle = 0; cycle < 70; ++cycle) {
        CapHandle process = kNullCap, thread = kNullCap;
        uint64_t identity = 0;
        spawn_child(&process, &thread, &identity, "test.busy");
        const uint32_t pid = static_cast<uint32_t>(identity);
        check(pid <= 64, "identity carries a PID");
        check(previous[pid] != identity, "PID generation reused");
        previous[pid] = identity;
        check(!sys_kill(process), "cycle kill");
        await_exit(identity, -1, "cycle exit identity");
    }
    print("generation reuse: identities remain unique per PID slot\n");
}

// ---------------------------------------------------------------------------
// 2. Simultaneous exits: every record is drained exactly once.
// ---------------------------------------------------------------------------
static void test_simultaneous_exits() {
    constexpr uint32_t kBatch = 8;
    CapHandle processes[kBatch]{};
    CapHandle threads[kBatch]{};
    uint64_t identities[kBatch]{};
    for (uint32_t i = 0; i < kBatch; ++i) {
        spawn_child(&processes[i], &threads[i], &identities[i], "test.busy");
    }
    for (uint32_t i = 0; i < kBatch; ++i) {
        check(!sys_kill(processes[i]), "batch kill");
    }
    uint32_t seen = 0;
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (seen != (1u << kBatch) - 1 && mx_clock_ns() < deadline) {
        uint64_t got = 0;
        long status = 0;
        if (poll_exit(&got, &status)) {
            uint32_t index = 0;
            for (; index < kBatch; ++index) if (identities[index] == got) break;
            check(index < kBatch, "foreign exit event");
            check(!(seen & (1u << index)), "duplicate exit event");
            check(status == -1, "exit status");
            seen |= 1u << index;
        }
        sys_yield();
    }
    check(seen == (1u << kBatch) - 1, "simultaneous exit lost");
    print("simultaneous exits: all drained once\n");
}

// ---------------------------------------------------------------------------
// 3. Shared policy: planned stops are exempt, crashes back off exponentially.
// ---------------------------------------------------------------------------
static void test_restart_policy() {
    const uint64_t ids[3] = {0, 0x1234'5678'9abcull, 0};
    check(match_identity(ids, 3, 0x1234'5678'9abcull) == 1, "identity matched");
    check(match_identity(ids, 3, 0x999) == 3, "unknown identity unmapped");
    check(match_identity(ids, 3, 0) == 3, "zero identity never matched");

    // A deliberate restart is immediate and is not a failure.
    const auto planned = restart_decision(true, true, false, false, 0);
    check(planned.restart && !planned.failed && planned.delay_ns == 0, "planned restart");
    // A deliberate stop leaves the service down.
    check(!restart_decision(true, false, true, false, 0).restart, "planned stop");
    // A "never" service that exits on its own is not auto-restarted.
    check(!restart_decision(false, true, false, false, 0).restart, "never restarts");

    // Repeated crashes back off, strictly growing then saturating.
    uint64_t previous = 0;
    for (uint32_t failures = 0; failures < kFailureCap; ++failures) {
        const auto crash = restart_decision(false, true, true, false, failures);
        check(crash.restart && crash.failed, "crash restarted and charged");
        check(crash.delay_ns == backoff_ns(failures + 1), "crash backoff schedule");
        check(crash.delay_ns > previous, "backoff grows");
        previous = crash.delay_ns;
    }
    check(backoff_ns(kFailureCap) == backoff_ns(kFailureCap + 5), "backoff saturates");
    print("restart policy: planned stops exempt, crashes back off\n");
}

// ---------------------------------------------------------------------------
// 4. Steady state: a dependency death stops and relaunches its dependents.
//    This mirrors userland/init's supervise_services() over real processes using
//    the shared policy, so a broken dependency closure fails the fixture.
// ---------------------------------------------------------------------------
struct Service {
    uint32_t depends;
    CapHandle process, thread;
    uint64_t identity;
    bool automatic, ready, desired, restart_pending, stopping;
    uint32_t failures;
    uint64_t retry_at;
};

static Service g_services[kMaxServices];
static uint32_t g_service_count = 0;

static uint32_t ready_mask() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < g_service_count; ++i) if (g_services[i].ready) mask |= 1u << i;
    return mask;
}
static bool dependencies_ready(const Service& svc) {
    return (svc.depends & ~ready_mask()) == 0;
}

static void drain_exits() {
    for (;;) {
        uint64_t got = 0;
        long status = 0;
        if (!poll_exit(&got, &status)) break;
        uint64_t identities[kMaxServices];
        for (uint32_t i = 0; i < g_service_count; ++i) identities[i] = g_services[i].identity;
        const uint32_t index = match_identity(identities, g_service_count, got);
        if (index == g_service_count) continue;
        Service& svc = g_services[index];
        svc.process = svc.thread = kNullCap;
        svc.identity = 0;
        svc.ready = false;
        const auto decision = restart_decision(svc.stopping, svc.desired, svc.automatic,
                                               svc.restart_pending, svc.failures);
        svc.stopping = false;
        if (decision.failed && svc.failures < kFailureCap) ++svc.failures;
        svc.restart_pending = decision.restart;
        if (decision.delay_ns) svc.retry_at = mx_clock_ns() + decision.delay_ns;
    }
}

static bool stop_mask(uint32_t mask) {
    for (uint32_t i = 0; i < g_service_count; ++i) {
        Service& svc = g_services[i];
        if (!(mask & (1u << i))) continue;
        svc.ready = false;
        svc.restart_pending = svc.desired;
        if (!svc.process) continue;
        svc.stopping = true;
        check(!sys_kill(svc.process), "dependent kill");
    }
    const uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline) {
        drain_exits();
        bool stopped = true;
        for (uint32_t i = 0; i < g_service_count; ++i) {
            if ((mask & (1u << i)) && g_services[i].process) stopped = false;
        }
        if (stopped) return true;
        sys_yield();
    }
    return false;
}

static bool supervise() {
    drain_exits();
    bool progress = false;
    uint32_t affected = 0;
    for (uint32_t i = 0; i < g_service_count; ++i) {
        if (!g_services[i].process && g_services[i].restart_pending) affected |= 1u << i;
    }
    if (!affected) return progress;
    uint32_t dependencies[kMaxServices];
    for (uint32_t i = 0; i < g_service_count; ++i) dependencies[i] = g_services[i].depends;
    const uint32_t dependents = dependent_closure(dependencies, g_service_count, affected) & ~affected;
    for (uint32_t i = 0; i < g_service_count; ++i) {
        if ((dependents & (1u << i)) && g_services[i].process) {
            check(stop_mask(dependents), "dependents stopped");
            break;
        }
    }
    for (uint32_t i = 0; i < g_service_count; ++i) {
        Service& svc = g_services[i];
        if (!svc.process && svc.desired && svc.restart_pending && mx_clock_ns() >= svc.retry_at) {
            spawn_child(&svc.process, &svc.thread, &svc.identity, "test.busy");
            svc.ready = false;
        }
    }
    for (uint32_t i = 0; i < g_service_count; ++i) {
        Service& svc = g_services[i];
        if (svc.process && !svc.ready && svc.desired && svc.restart_pending &&
            dependencies_ready(svc)) {
            svc.ready = true;
            svc.restart_pending = false;
            svc.failures = 0;
            progress = true;
        }
    }
    return progress;
}

static void test_dependency_restart() {
    g_service_count = 3;  // 0: root, 1: depends on 0, 2: depends on 1
    for (uint32_t i = 0; i < g_service_count; ++i) {
        g_services[i] = Service{};
        g_services[i].desired = true;
        g_services[i].automatic = true;
        g_services[i].restart_pending = true;  // every desired service wants to run
    }
    g_services[1].depends = 1u << 0;
    g_services[2].depends = 1u << 1;

    uint64_t deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline && ready_mask() != (1u << g_service_count) - 1) {
        supervise();
        sys_yield();
    }
    check(ready_mask() == (1u << g_service_count) - 1, "initial dependency launch");
    const uint64_t root = g_services[0].identity;
    const uint64_t dependent1 = g_services[1].identity;
    const uint64_t dependent2 = g_services[2].identity;

    check(!sys_kill(g_services[0].process), "root kill");
    // Wait for the restart to settle: the first supervise() drains the death,
    // so the loop must run before it can observe the cleared mask.
    bool restarted = false;
    deadline = mx_clock_ns() + 3000000000ull;
    while (mx_clock_ns() < deadline) {
        supervise();
        if (ready_mask() == (1u << g_service_count) - 1 &&
            g_services[0].identity != root &&
            g_services[1].identity != dependent1 &&
            g_services[2].identity != dependent2) {
            restarted = true;
            break;
        }
        sys_yield();
    }
    check(restarted, "dependency restart");
    print("dependency restart: dependents stopped and relaunched\n");
}

extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map();
    supervisor = mochi::boot::require_handle(boot, "supervisor");
    cpu_count = mochi::boot::find(boot, "cpu.count")->aux0;
    auto archive = mochi::boot::require_handle(boot, "boot.initrd");
    check(!sys_map_page(archive, 0x20000000, RIGHT_READ), "bundle mapping");
    bundle.data = reinterpret_cast<const uint8_t*>(0x20000000);
    bundle.size = mochi::boot::find(boot, "boot.initrd")->aux0;
    (void)libk::copy_n(libk::as_bytes(&bundle.header, sizeof(bundle.header)), libk::as_bytes(bundle.data, sizeof(bundle.header)), sizeof(bundle.header));
    print("supervision regression started\n");

    test_generation_matching();
    test_generation_reuse();
    test_simultaneous_exits();
    test_restart_policy();
    test_dependency_restart();

    print("supervision regression passed\n");
    while (true) {
        sys_yield();
    }
}

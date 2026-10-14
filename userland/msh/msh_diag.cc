// Serial-friendly, read-only diagnostics. Everything uses the shell's terminal
// route, so CDC receives the same report as the framebuffer console.
#include "msh.h"
#include "libk/printf.h"
#include "midix/abi/debug_counters.h"
#include "midix/abi/diagnostics.h"

namespace {
template <typename... Args>
void line(const char* format, Args... args) {
    char buffer[256];
    // Include CRLF in one terminal write, leaving room for the formatter's NUL.
    const int length = libk::snprintf(buffer, sizeof(buffer) - 2, format, args...);
    if (length < 0) { g.output_failed = true; return; }
    const auto count = static_cast<uint32_t>(length);
    uint32_t used = count < sizeof(buffer) - 2 ? count : sizeof(buffer) - 3;
    if (used != count) g.output_failed = true;
    buffer[used++] = '\r'; buffer[used++] = '\n';
    put_bytes(buffer, used);
}

void decimal(const char* name, uint64_t value) {
    line("%s=%llu", name, static_cast<unsigned long long>(value));
}
void hex(const char* name, uint64_t value) {
    line("%s=0x%llx", name, static_cast<unsigned long long>(value));
}
void error(const char* what, int64_t status) {
    line("%s: error=%lld", what, static_cast<long long>(status));
}

bool system_info(MxDiagSystem& info) {
    const auto rc = sys_debug_read(MX_DIAG_SYSTEM, 0, &info, sizeof(info));
    if (rc != sizeof(info)) {
        error("system diagnostics unavailable", rc);
        return false;
    }
    if (info.version != MX_DIAG_VERSION) {
        decimal("unsupported diagnostic version", info.version);
        return false;
    }
    return true;
}

int cpu_info(const MxDiagSystem& info) {
    const char* board = "unknown";
    switch (info.board) {
    case MX_DIAG_BOARD_ANNE: board = "huawei-anne"; break;
    case MX_DIAG_BOARD_QEMU: board = "qemu-virt"; break;
    case MX_DIAG_BOARD_SWITCH: board = "switch-t210"; break;
    }
    line("board=%s diag.version=%u", board, info.version);
    decimal("cpu.sampled", info.sampled_cpu);
    decimal("host.current_el", info.current_el);
    hex("cpu.online_mask", info.online_mask);
    hex("cpu.midr", info.midr);
    hex("cpu.mpidr", info.mpidr);
    hex("cpu.pfr0", info.pfr0);
    decimal("el2.hardware_field", (info.pfr0 >> 8) & 15);
    hex("cpu.mmfr0", info.mmfr0);
    hex("cache.sctlr", info.sctlr);
    hex("cache.tcr", info.tcr);
    hex("cache.mair", info.mair);
    hex("cache.ctr", info.ctr);
    decimal("timer.reported_hz", info.counter_reported_hz);
    decimal("timer.effective_hz", info.counter_effective_hz);
    // Walk the kernel-advertised limit, including offline/unentered slots.
    // A fixed ABI bound prevents a malformed response causing endless output.
    if (info.cpu_limit > 64) { putln("invalid CPU limit"); return 1; }
    int result = 0;
    for (uint32_t cpu = 0; cpu < info.cpu_limit; ++cpu) {
        MxDiagCpu state{};
        const auto rc = sys_debug_read(MX_DIAG_CPU, cpu, &state, sizeof(state));
        if (rc != sizeof(state) || state.version != MX_DIAG_VERSION || state.cpu != cpu) {
            error("CPU diagnostics unavailable", rc);
            result = 1;
            continue;
        }
        line("cpu%u online=%llu mpidr=0x%llx boot_entry_el=%llu monitor_ready=%llu",
             cpu, static_cast<unsigned long long>(state.online),
             static_cast<unsigned long long>(state.mpidr),
             static_cast<unsigned long long>(state.boot_entry_el),
             static_cast<unsigned long long>(state.monitor_ready));
        line("cpu%u switches=%llu preempts=%llu ticks=%llu ipis=%llu", cpu,
             static_cast<unsigned long long>(state.context_switches),
             static_cast<unsigned long long>(state.timer_preempts),
             static_cast<unsigned long long>(state.timer_ticks),
             static_cast<unsigned long long>(state.ipis_handled));
    }
    putln("EL2 hardware support does not imply an installed Ame monitor.");
    return result;
}

struct Counter { uint64_t id; const char* name; };
constexpr Counter counters[] = {
    {DBG_SYSCALL_COUNT, "syscalls"}, {DBG_IPC_SYSCALL_COUNT, "ipc.syscalls"},
    {DBG_CONTEXT_SWITCHES, "sched.switches"}, {DBG_DEBUG_PUTS, "debug.puts"},
    {DBG_EL0_FAULTS, "el0.faults"}, {DBG_DOORBELL_SLEEPS, "doorbell.sleeps"},
    {DBG_DOORBELL_WAKES, "doorbell.wakes"}, {DBG_TIMER_TICKS, "timer.ticks"},
    {DBG_DOORBELL_WAIT_CALLS, "doorbell.waits"}, {DBG_DOORBELL_WAIT_IMMED, "doorbell.immediate"},
    {DBG_NOTIFY_CALLS, "notify.calls"}, {DBG_NOTIFY_NO_WAITER, "notify.no_waiter"},
    {DBG_NOTIFY_WOKE, "notify.woke"}, {DBG_YIELD_COUNT, "sched.yields"},
    {DBG_BLOCK_SWITCHES, "sched.blocks"}, {DBG_TIMER_PREEMPTS, "sched.preempts"},
    {DBG_EXIT_SWITCHES, "sched.exits"}, {DBG_ONLINE_CPUS, "cpu.online_count"},
    {DBG_CPU0_CTX_SWITCHES, "cpu0.switches"}, {DBG_CPU1_CTX_SWITCHES, "cpu1.switches"},
    {DBG_CPU0_TIMER_PREEMPTS, "cpu0.preempts"}, {DBG_CPU1_TIMER_PREEMPTS, "cpu1.preempts"},
    {DBG_LOCAL_WAKES, "sched.local_wakes"}, {DBG_REMOTE_WAKES, "sched.remote_wakes"},
    {DBG_IPI_SENT, "ipi.sent"}, {DBG_IPI_HANDLED, "ipi.handled"},
    {DBG_PENDING_WAKE_DRAINS, "sched.wake_drains"}, {DBG_DOORBELL_LOCAL_WAKES, "doorbell.local_wakes"},
    {DBG_DOORBELL_REMOTE_WAKES, "doorbell.remote_wakes"}, {DBG_FAIRNESS_LAG, "sched.fairness_lag"},
    {DBG_PRINCIPAL_TRUSTED_RT, "sched.trusted_runtime"}, {DBG_PRINCIPAL_SANDBOX_RT, "sched.sandbox_runtime"},
    {DBG_WAKE_DOORBELL, "sched.wake_doorbell"}, {DBG_WAKE_REMOTE, "sched.wake_remote"},
    {DBG_WAKE_RESUME, "sched.wake_resume"}, {DBG_IRQ_DELIVERED, "irq.delivered"},
    {DBG_IRQ_BOUND, "irq.bound"}, {DBG_IRQ_SELFTEST, "irq.selftest"}, {DBG_BOOT_CS, "boot.centiseconds"},
    {200, "objects.live"}, {201, "memory.allocated_pages"},
    {202, "shared_arena.attached"}, {203, "shared_arena.dma_registered"},
};
// All existing VM debug IDs 32..63, including assignment counters which the
// older libdango header does not yet name. Keep their wire order explicit.
constexpr const char* vm_counters[] = {
    "create_calls", "created", "destroyed", "memory_created", "memory_mapped", "memory_unmapped",
    "stage2_tables", "stage2_pages", "vcpu_created", "vcpu_configured", "vcpu_runs", "vcpu_notimpl",
    "channels", "errors", "host_el", "el2_available", "vhe_available", "guest_boot_stub",
    "guest_agent_start", "guest_linux_hello", "wfx_exits", "irq_exits", "unsupported_exits",
    "device_created", "device_active", "device_destroyed", "device_bar_mapped", "device_irq_routes",
    "device_irq_injected", "device_mediated_deny", "device_bme_enable", "device_quarantine",
};
static_assert(sizeof(vm_counters) / sizeof(vm_counters[0]) == 32);

int runtime_info() {
    int result = 0;
    putln("--- counters (live samples) ---");
    for (const auto& counter : counters) {
        const auto value = sys_debug_counter(counter.id);
        if (value < 0) { error(counter.name, value); result = 1; }
        else decimal(counter.name, static_cast<uint64_t>(value));
    }
    for (uint32_t i = 0; i < 32; ++i) {
        const auto value = sys_debug_counter(dango::VM_DEBUG_CREATE_CALLS + i);
        line("vm.%s=%lld", vm_counters[i], static_cast<long long>(value));
        if (value < 0) result = 1;
    }
    putln("--- shell endpoints ---");
    decimal("shell.terminal", g.have_terminal);
    decimal("shell.filesystem", g.have_fs);
    decimal("shell.service_control", g.have_svcctl);
    putln("--- boot resources ---");
    if (cmd_bootinfo(0, nullptr)) result = 1;
    if (cmd_cap(0, nullptr)) result = 1;
    if (cmd_ring(0, nullptr)) result = 1;
    if (cmd_doorbell(0, nullptr)) result = 1;
    putln("--- services ---");
    if (cmd_svc(0, nullptr)) result = 1;
    return result;
}

int boot_log(const MxDiagSystem& info) {
    putln("--- retained boot log ---");
    if (!info.boot_log_capacity) {
        putln("retained boot log unavailable on this board");
        return 0;
    }
    if (info.boot_log_size > info.boot_log_capacity) {
        putln("invalid retained log size"); return 1;
    }
    decimal("boot_log.bytes", info.boot_log_size);
    decimal("boot_log.capacity", info.boot_log_capacity);
    if (info.boot_log_size == info.boot_log_capacity)
        putln("boot log capacity reached; later records may be missing");
    // Snapshot length was captured before printing anything. Never follow a
    // moving end or consume records. A debug-console fallback can itself append
    // to this log, whereas the normal terminal/CDC route keeps output separate.
    char bytes[MX_DIAG_READ_MAX];
    bool previous_cr = false;
    for (uint64_t offset = 0; offset < info.boot_log_size;) {
        const uint64_t left = info.boot_log_size - offset;
        const uint64_t requested = left < sizeof(bytes) ? left : sizeof(bytes);
        const auto count = sys_debug_read(MX_DIAG_BOOT_LOG, offset, bytes, requested);
        if (count <= 0 || static_cast<uint64_t>(count) > requested) {
            error("retained log read incomplete", count); return 1;
        }
        // Early logs use LF; the raw serial terminal needs CRLF. Preserve an
        // existing CRLF even when it straddles two reads, and batch byte runs.
        uint32_t start = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
            if (bytes[i] == '\n' && !previous_cr) {
                put_bytes(bytes + start, i - start);
                put("\r");
                start = i;
            }
            previous_cr = bytes[i] == '\r';
        }
        put_bytes(bytes + start, static_cast<uint32_t>(count) - start);
        offset += static_cast<uint64_t>(count);
    }
    put("\r\n");
    return 0;
}
}

int cmd_diag(int argc, char** argv) {
    const bool all = argc < 2 || (argc == 2 && streq(argv[1], "all"));
    const bool el2 = argc == 2 && streq(argv[1], "el2");
    const bool log = argc == 2 && streq(argv[1], "log");
    if (argc > 2 || (!all && !el2 && !log)) {
        putln("usage: diag [all|el2|log]"); return 1;
    }
    g.output_failed = false;
    putln("=== diag begin ===");
    MxDiagSystem info{};
    int result = system_info(info) ? 0 : 1;
    if (!result) {
        if (all || el2) result |= cpu_info(info);
        if (all) result |= runtime_info();
        if (all || log) result |= boot_log(info);
    }
    if (g.output_failed) result = 1;
    line("=== diag end status=%u ===", static_cast<unsigned>(result));
    return g.output_failed ? 1 : result;
}

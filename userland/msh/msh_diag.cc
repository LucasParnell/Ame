// Serial-friendly diagnostics. Explicit EL3 operations can invoke firmware.
// Everything uses the shell's terminal
// route, so CDC receives the same report as the framebuffer console.
#include "msh.h"
#include "libk/printf.h"
#include "midix/abi/debug_counters.h"
#include "midix/abi/diagnostics.h"
#include "mochi/gpu_diag.h"
#include "midix/abi/cpu_limits.h"
#include "midix/abi/syscall.h"

namespace {
template <typename... Args> void line(const char* format, Args... args) {
    char buffer[256];
    // Include CRLF in one terminal write, leaving room for the formatter's NUL.
    const int length = libk::snprintf(buffer, sizeof(buffer) - 2, format, args...);
    if (length < 0) {
        g.output_failed = true;
        return;
    }
    const auto count = static_cast<uint32_t>(length);
    uint32_t used = count < sizeof(buffer) - 2 ? count : sizeof(buffer) - 3;
    if (used != count) {
        g.output_failed = true;
    }
    buffer[used++] = '\r';
    buffer[used++] = '\n';
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

// Cold diagnostic storage stays outside msh's 4 KiB stack. Sampling only reads
// existing accounting; sorting an index column leaves the 64-byte rows intact.
struct RuntimeSample {
    MxDiagProcessRuntime rows[MX_DIAG_PROCESS_LIMIT + 1];
    uint64_t sampled_ns, online_mask, skew_ns;
};
RuntimeSample runtime_before{}, runtime_after{};
uint64_t runtime_delta[MX_DIAG_PROCESS_LIMIT + 1];
uint8_t runtime_order[MX_DIAG_PROCESS_LIMIT];

bool read_runtime(RuntimeSample& sample) {
    libk::zero(sample);
    for (uint32_t first = 1; first <= MX_DIAG_PROCESS_LIMIT; first += MX_DIAG_RUNTIME_ROWS) {
        MxDiagRuntimeBatch batch{};
        const auto rc = sys_debug_read(MX_DIAG_RUNTIME, first, &batch, sizeof(batch));
        const auto next = first + MX_DIAG_RUNTIME_ROWS <= MX_DIAG_PROCESS_LIMIT
                              ? first + MX_DIAG_RUNTIME_ROWS
                              : 0;
        if (rc != sizeof(batch) || batch.version != MX_DIAG_VERSION || batch.first_pid != first ||
            batch.count > MX_DIAG_RUNTIME_ROWS || batch.next_pid != next) {
            return false;
        }
        if (first == 1) {
            sample.sampled_ns = batch.sampled_ns;
            sample.online_mask = batch.online_mask;
        }
        if (batch.online_mask != sample.online_mask || batch.sampled_ns < sample.sampled_ns) {
            return false;
        }
        sample.skew_ns = batch.sampled_ns - sample.sampled_ns;
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto& row = batch.rows[i];
            if (row.pid < first || row.pid >= first + MX_DIAG_RUNTIME_ROWS ||
                row.pid > MX_DIAG_PROCESS_LIMIT || static_cast<uint32_t>(row.identity) != row.pid ||
                !row.identity || row.reserved || row.name[31] || sample.rows[row.pid].identity) {
                return false;
            }
            sample.rows[row.pid] = row;
        }
    }
    return sample.online_mask != 0;
}

int runtime_profile(int argc, char** argv) {
    uint32_t interval_ms = 5000;
    if (argc == 3) {
        interval_ms = 0;
        for (const char* p = argv[2]; *p; ++p) {
            if (*p < '0' || *p > '9' || interval_ms > 30000) {
                putln("usage: diag runtime [100..30000 milliseconds]");
                return 1;
            }
            interval_ms = interval_ms * 10 + static_cast<uint32_t>(*p - '0');
        }
    }
    if (argc > 3 || interval_ms < 100 || interval_ms > 30000) {
        putln("usage: diag runtime [100..30000 milliseconds]");
        return 1;
    }
    if (!read_runtime(runtime_before)) {
        putln("runtime snapshot unavailable");
        return 1;
    }
    // Use the existing input doorbell as a sleep source. Early input wakes are
    // harmless; preserve queued bytes and continue sleeping until the deadline.
    const auto deadline = mx_clock_ns() + static_cast<uint64_t>(interval_ms) * 1000000;
    while (mx_clock_ns() < deadline) {
        const auto wake = g.term.endpoint.rx_doorbell;
        const auto expected = mx_doorbell_read(wake);
        const auto rc = mx_doorbell_wait_until(wake, expected, deadline);
        if (rc != MX_OK) {
            error("runtime sample wait", rc);
            return 1;
        }
    }
    if (!read_runtime(runtime_after) || runtime_after.sampled_ns <= runtime_before.sampled_ns ||
        runtime_after.online_mask != runtime_before.online_mask) {
        putln("runtime snapshot changed or unavailable");
        return 1;
    }
    const auto wall = runtime_after.sampled_ns - runtime_before.sampled_ns;
    uint64_t total = 0;
    uint32_t count = 0, excluded = 0;
    for (uint32_t pid = 1; pid <= MX_DIAG_PROCESS_LIMIT; ++pid) {
        const auto& before = runtime_before.rows[pid];
        const auto& after = runtime_after.rows[pid];
        if (!after.identity && !before.identity) {
            continue;
        }
        if (!after.identity || before.identity != after.identity || after.flags ||
            after.runtime_ns < before.runtime_ns) {
            ++excluded;
            continue;
        }
        runtime_delta[pid] = after.runtime_ns - before.runtime_ns;
        total += runtime_delta[pid];
        uint32_t at = count++;
        while (at && runtime_delta[runtime_order[at - 1]] < runtime_delta[pid]) {
            runtime_order[at] = runtime_order[at - 1];
            --at;
        }
        runtime_order[at] = static_cast<uint8_t>(pid);
    }
    const auto cpus = static_cast<uint32_t>(__builtin_popcountll(runtime_after.online_mask));
    // All reporting is after the sampled interval, excluding framebuffer/USB
    // report rendering from this CPU ranking. Kernel/syscall time while a
    // process runs is charged to that process; idle/IRQ-only work is separate.
    putln("=== runtime begin ===");
    decimal("runtime.wall_ns", wall);
    decimal("runtime.online_cpus", cpus);
    decimal("runtime.total_cpu_ns", total);
    decimal("runtime.excluded_processes", excluded);
    decimal("runtime.snapshot_skew_ns", runtime_after.skew_ns);
    const auto total_pct = total * 10000 / wall;
    line("runtime.total_one_cpu_pct=%llu.%02llu", static_cast<unsigned long long>(total_pct / 100),
         static_cast<unsigned long long>(total_pct % 100));
    const auto capacity_pct = total_pct / cpus;
    line("runtime.total_capacity_pct=%llu.%02llu",
         static_cast<unsigned long long>(capacity_pct / 100),
         static_cast<unsigned long long>(capacity_pct % 100));
    for (uint32_t i = 0; i < count; ++i) {
        const auto pid = runtime_order[i];
        const auto pct = runtime_delta[pid] * 10000 / wall;
        line("runtime.process pid=%u name=%s cpu_ns=%llu one_cpu_pct=%llu.%02llu", pid,
             runtime_after.rows[pid].name, static_cast<unsigned long long>(runtime_delta[pid]),
             static_cast<unsigned long long>(pct / 100),
             static_cast<unsigned long long>(pct % 100));
    }
    putln("=== runtime end status=0 ===");
    return g.output_failed ? 1 : 0;
}

bool parse_hex(const char* s, uint64_t* out) {
    if (!s || !*s) {
        return false;
    }
    uint64_t v = 0;
    uint32_t digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (; *s; ++s) {
        uint32_t d;
        const char c = *s;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        if (++digits > 16) {
            return false;
        }
        v = (v << 4) | d;
    }
    if (digits == 0) {
        return false;
    }
    *out = v;
    return true;
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
    case MX_DIAG_BOARD_ANNE:
        board = "huawei-anne";
        break;
    case MX_DIAG_BOARD_QEMU:
        board = "qemu-virt";
        break;
    case MX_DIAG_BOARD_SWITCH:
        board = "switch-t210";
        break;
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
    if (info.cpu_limit > 64) {
        putln("invalid CPU limit");
        return 1;
    }
    int result = 0;
    for (uint32_t cpu = 0; cpu < info.cpu_limit; ++cpu) {
        MxDiagCpu state{};
        const auto rc = sys_debug_read(MX_DIAG_CPU, cpu, &state, sizeof(state));
        if (rc != sizeof(state) || state.version != MX_DIAG_VERSION || state.cpu != cpu) {
            error("CPU diagnostics unavailable", rc);
            result = 1;
            continue;
        }
        line("cpu%u online=%llu mpidr=0x%llx boot_entry_el=%llu monitor_ready=%llu", cpu,
             static_cast<unsigned long long>(state.online),
             static_cast<unsigned long long>(state.mpidr),
             static_cast<unsigned long long>(state.boot_entry_el),
             static_cast<unsigned long long>(state.monitor_ready));
        line("cpu%u switches=%llu preempts=%llu ticks=%llu ipis=%llu", cpu,
             static_cast<unsigned long long>(state.context_switches),
             static_cast<unsigned long long>(state.timer_preempts),
             static_cast<unsigned long long>(state.timer_ticks),
             static_cast<unsigned long long>(state.ipis_handled));
        MxDiagVmProgress progress{};
        if (sys_debug_read(MX_DIAG_VM_PROGRESS, cpu, &progress, sizeof(progress)) == sizeof(progress)) {
            line("cpu%u vm.stage=%llu sequence=%llu sampled_ns=%llu detail0=0x%llx detail1=0x%llx", cpu,
                 static_cast<unsigned long long>(progress.stage),
                 static_cast<unsigned long long>(progress.sequence),
                 static_cast<unsigned long long>(progress.sampled_ns),
                 static_cast<unsigned long long>(progress.detail0),
                 static_cast<unsigned long long>(progress.detail1));
        }
    }
    putln("EL2 hardware support does not imply an installed Ame monitor.");
    return result;
}

int el3_info(bool canary = false) {
    MxDiagEl3 info{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, canary ? 1 : 0, &info, sizeof(info));
    if (rc != sizeof(info)) {
        error("EL3 diagnostics unavailable", rc);
        return 1;
    }
    decimal("el3.available", info.available);
    if (!info.available) {
        putln("EL3 RPMB service unavailable on this board.");
        return 1;
    }
    hex("el3.shared_base", info.shared_base);
    hex("el3.rpmb_request", info.request_address);
    hex("el3.rpmb_request_size", info.request_size);
    hex("el3.bl31_base", info.bl31_base);
    hex("el3.bl31_size", info.bl31_size);
    putln("el3.rpmb_callback=undefined_in_hi6250_linux");
    for (uint32_t i = 0; i < 4; ++i) {
        line("el3.rpmb_call_count.x%u=0x%llx", i,
             static_cast<unsigned long long>(info.call_count[i]));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        line("el3.rpmb_uid.x%u=0x%llx", i, static_cast<unsigned long long>(info.uid[i]));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        line("el3.rpmb_version.x%u=0x%llx", i,
             static_cast<unsigned long long>(info.service_version[i]));
    }
    if (canary) {
        putln("EL3 registration profile: stock-log size/reliable-write; EXT_CSD region bytes "
              "assumed.");
        hex("el3.request_address_result", info.request_address_result);
        // A nonzero secure-service result is a failed canary even though the
        // diagnostic read itself completed successfully.
        if (info.request_address_result != 0) {
            putln("EL3 canary rejected; no callback or payload was used.");
            return 1;
        }
        putln("EL3 canary registered the request address only; no callback or payload was used.");
    } else {
        putln("EL3 probe queried metadata only; no shared request or payload was used.");
    }
    return 0;
}

int el3_recon() {
    MxDiagEl3Recon recon{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 2, &recon, sizeof(recon));
    if (rc != sizeof(recon)) {
        error("EL3 recon unavailable", rc);
        return 1;
    }
    line("el3r.window=0x%llx size=0x%llx", static_cast<unsigned long long>(recon.window_base),
         static_cast<unsigned long long>(recon.window_size));
    hex("el3r.base_6000", recon.base_6000);
    hex("el3r.slot_6c30", recon.slot_6c30);
    hex("el3r.slot_6c38", recon.slot_6c38);
    hex("el3r.slot_6c40", recon.slot_6c40);
    hex("el3r.slot_6c48", recon.slot_6c48);
    hex("el3r.raw_6c42", recon.status_6c42);
    line("el3r.bl31_range=0x%llx-0x%llx", static_cast<unsigned long long>(recon.bl31_lo),
         static_cast<unsigned long long>(recon.bl31_hi));
    decimal("el3r.ptr_count", recon.ptr_count);
    const uint32_t shown = recon.ptr_count < 8 ? static_cast<uint32_t>(recon.ptr_count) : 8;
    for (uint32_t i = 0; i < shown; ++i) {
        line("el3r.ptr[%u]=0x%llx:0x%llx", i,
             static_cast<unsigned long long>(recon.ptr_slots[i][0]),
             static_cast<unsigned long long>(recon.ptr_slots[i][1]));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        line("el3r.crumb%u=0x%llx", i, static_cast<unsigned long long>(recon.crumb[i]));
    }
    hex("el3r.isar0", recon.isar0);
    putln("EL3 recon read only; no writes and no SMC were issued.");
    return 0;
}

int el3_done() {
    MxDiagEl3Done done{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 3, &done, sizeof(done));
    if (rc != sizeof(done)) {
        error("EL3 done-probe unavailable", rc);
        return 1;
    }
    decimal("el3d.aborted", done.aborted);
    hex("el3d.registered", done.registered);
    hex("el3d.slot_pre", done.slot_pre);
    hex("el3d.status_pre", done.status_pre);
    hex("el3d.slot_after_write", done.slot_after_write);
    hex("el3d.status_after_write", done.status_after_write);
    hex("el3d.call.x0", done.call_x0);
    hex("el3d.call.x1", done.call_x1);
    hex("el3d.call.x2", done.call_x2);
    hex("el3d.call.x3", done.call_x3);
    hex("el3d.slot_post", done.slot_post);
    hex("el3d.status_post", done.status_post);
    decimal("el3d.status_cleared", done.cleared);
    line("el3d.ticks_delta=%llu",
         static_cast<unsigned long long>(done.tick_after - done.tick_before));
    if (done.aborted != 0) {
        putln("EL3 done-probe aborted before the call.");
        return 1;
    }
    return done.call_x0 == 0 ? 0 : 1;
}

struct RpmbWorker {
    uint32_t state; // 0 unused, 1 queued/running, 2 returned
    uint32_t cancel;
    uint64_t cpu;
    int64_t rc;
    uint32_t continuous;
    CapHandle wake;
};
RpmbWorker rpmb_workers[2]{};

int64_t rpmb_serve(RpmbWorker& worker) {
    // A private timed doorbell avoids consuming terminal input or spinning a
    // CPU between SGIs. SGI 5 is observed through the kernel's atomic counter.
    mx_handle_t wake = kNullCap;
    auto status = mx_doorbell_create(&wake);
    if (status != MX_OK) {
        return status;
    }
    __atomic_store_n(&worker.wake, wake, __ATOMIC_RELEASE);
    int64_t result = 0;
    while (!__atomic_load_n(&worker.cancel, __ATOMIC_ACQUIRE)) {
        MxDiagEl3Rpmb info{};
        info.action = MX_RPMB_STATUS;
        auto rc = sys_debug_read(MX_DIAG_EL3, 29, &info, sizeof(info));
        if (rc != sizeof(info) || info.version != MX_DIAG_VERSION) {
            result = rc < 0 ? rc : MX_ERR_INVALID;
            break;
        }
        if (!info.armed || !info.registered || !info.storage_enabled || info.registering ||
            info.provision_pending) {
            result = -static_cast<int64_t>(MX_RPMB_BUSY);
            break;
        }
        if (info.done_returned && info.done_result) {
            result = -static_cast<int64_t>(MX_RPMB_COMPLETION_FAILED);
            break;
        }
        if (info.completed_events > info.events || info.events - info.completed_events > 1) {
            result = -static_cast<int64_t>(MX_RPMB_AMBIGUOUS_REQUEST);
            break;
        }
        if (info.events != info.completed_events) {
            info = {};
            info.action = MX_RPMB_COMPLETE;
            rc = sys_debug_read(MX_DIAG_EL3, 29, &info, sizeof(info));
            if (rc != sizeof(info) || info.version != MX_DIAG_VERSION ||
                info.result != MX_RPMB_OK) {
                result = rc == sizeof(info)
                             ? (info.version == MX_DIAG_VERSION ? -static_cast<int64_t>(info.result)
                                                                : MX_ERR_INVALID)
                         : rc < 0 ? rc
                                  : MX_ERR_INVALID;
                break;
            }
            continue; // FF06 may have produced the next chunk before returning.
        }
        const auto expected = mx_doorbell_read(wake);
        if (__atomic_load_n(&worker.cancel, __ATOMIC_ACQUIRE)) {
            break;
        }
        status = mx_doorbell_wait_until(wake, expected, mx_clock_ns() + 1000000);
        if (status != MX_OK) {
            result = status;
            break;
        }
    }
    __atomic_store_n(&worker.wake, kNullCap, __ATOMIC_RELEASE);
    sys_cap_revoke(wake);
    return result;
}

void rpmb_worker(uint64_t index) {
    auto& worker = rpmb_workers[index];
    if (worker.continuous) {
        worker.rc = rpmb_serve(worker);
    } else if (!__atomic_load_n(&worker.cancel, __ATOMIC_ACQUIRE)) {
        MxDiagEl3Rpmb info{};
        info.action = index == 0 ? MX_RPMB_COUNTER : MX_RPMB_COMPLETE;
        const auto rc = sys_debug_read(MX_DIAG_EL3, 29, &info, sizeof(info));
        worker.rc = rc == sizeof(info) ? -static_cast<int64_t>(info.result) : rc;
    }
    __atomic_store_n(&worker.state, 2u, __ATOMIC_RELEASE);
    sys_thread_exit(0);
    for (;;) {
        sys_yield();
    }
}

bool rpmb_read(uint32_t action, MxDiagEl3Rpmb& info) {
    info = {};
    info.action = action;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 29, &info, sizeof(info));
    if (rc != sizeof(info) || info.version != MX_DIAG_VERSION) {
        error("RPMB lifecycle diagnostic unavailable", rc);
        return false;
    }
    return true;
}

void rpmb_report(const MxDiagEl3Rpmb& info) {
    constexpr const char* names[] = {"ok",
                                     "busy",
                                     "no_request",
                                     "registration_failed",
                                     "ambiguous_request",
                                     "stale_request",
                                     "already_called",
                                     "completion_failed",
                                     "bad_action",
                                     "storage_unavailable",
                                     "provision_failed",
                                     "already_provisioned",
                                     "provision_uncertain",
                                     "key_query_failed"};
    decimal("rpmb.result", info.result);
    line("rpmb.result_name=%s",
         info.result <= MX_RPMB_KEY_QUERY_FAILED ? names[info.result] : "invalid");
    decimal("rpmb.key_state", info.key_state);
    decimal("rpmb.provision_stage", info.provision_stage);
    decimal("rpmb.provision_pending", info.provision_pending);
    hex("rpmb.key_smc_x0", info.key_smc_result);
    hex("rpmb.key_transport_result", info.key_transport_result);
    hex("rpmb.key_response_result", info.key_response_result);
    decimal("rpmb.key_counter", info.key_counter);
    decimal("rpmb.emmc", info.storage_enabled);
    decimal("rpmb.completed_events", info.completed_events);
    decimal("rpmb.armed", info.armed);
    decimal("rpmb.registering", info.registering);
    decimal("rpmb.registered", info.registered);
    hex("rpmb.registration_x0", info.registration_result);
    decimal("rpmb.sgi5_events", info.events);
    decimal("rpmb.event_cpu", info.event_cpu);
    decimal("rpmb.trigger_started", info.trigger_started);
    decimal("rpmb.trigger_inflight", info.trigger_inflight);
    decimal("rpmb.trigger_returned", info.trigger_returned);
    hex("rpmb.trigger_x0", info.trigger_result);
    hex("rpmb.func_id", info.func_id);
    decimal("rpmb.request_state", info.request_state);
    hex("rpmb.frame_type", info.frame_type);
    hex("rpmb.key_status", info.key_status);
    hex("rpmb.request_status", info.request_status);
    hex("rpmb.exception_status", info.exception_status);
    decimal("rpmb.ff06_started", info.done_started);
    decimal("rpmb.ff06_inflight", info.done_inflight);
    decimal("rpmb.ff06_returned", info.done_returned);
    hex("rpmb.ff06_x0", info.done_result);
    hex("rpmb.ff06_storage_result", info.completion_error);
    if (info.result == MX_RPMB_NO_REQUEST) {
        putln("No SGI 5 notification received; FF06 was not called.");
    }
    if (info.trigger_returned && !info.events) {
        putln(
            "Counter SMC returned without SGI 5; retail firmware may disable this debug service.");
    }
    if (info.done_returned && !info.done_result) {
        line("FF06 returned after a real notification; storage result=0x%llx.",
             static_cast<unsigned long long>(info.completion_error));
    }
    for (unsigned i = 0; i < 2; ++i) {
        const auto& worker = rpmb_workers[i];
        const auto state = __atomic_load_n(&worker.state, __ATOMIC_ACQUIRE);
        line("rpmb.worker%u state=%u cpu=%llu", i, state,
             static_cast<unsigned long long>(worker.cpu));
        if (state == 2) {
            line("rpmb.worker%u result=%lld", i, static_cast<long long>(worker.rc));
        }
    }
}

int el3_rpmb(int argc, char** argv) {
    if (argc == 3 && streq(argv[2], "stop")) {
        auto& worker = rpmb_workers[1];
        if (__atomic_load_n(&worker.state, __ATOMIC_ACQUIRE) == 1 && worker.continuous) {
            __atomic_store_n(&worker.cancel, 1u, __ATOMIC_RELEASE);
            const auto wake = __atomic_load_n(&worker.wake, __ATOMIC_ACQUIRE);
            if (wake) {
                mx_doorbell_notify(wake);
            }
            putln("RPMB service stop requested; an active card/firmware call must return first.");
        } else {
            putln("RPMB service is not running.");
        }
        return 0;
    }
    const bool counter = argc == 4 && streq(argv[2], "counter");
    const bool complete = argc == 4 && streq(argv[2], "complete");
    const bool serve = argc == 4 && streq(argv[2], "serve");
    const bool provision = argc == 3 && streq(argv[2], "provision-key");
    uint32_t action = MX_RPMB_STATUS;
    if (argc == 3 && streq(argv[2], "arm")) {
        action = MX_RPMB_ARM;
    } else if (argc == 3 && streq(argv[2], "arm-emmc")) {
        action = MX_RPMB_ARM_EMMC;
    } else if (argc == 3 && streq(argv[2], "release")) {
        action = MX_RPMB_RELEASE;
    } else if (provision) {
        action = MX_RPMB_PROVISION_KEY;
    } else if (argc == 3 && streq(argv[2], "key-status")) {
        action = MX_RPMB_KEY_STATUS;
    } else if (!(argc == 3 && streq(argv[2], "status")) && !counter && !complete && !serve) {
        putln("usage: diag el3-rpmb "
              "arm|arm-emmc|key-status|status|release|provision-key|stop|counter <cpu>|complete "
              "<cpu>|serve <cpu>");
        return 1;
    }
    MxDiagEl3Rpmb info{};
    if (!counter && !complete && !serve) {
        if (!rpmb_read(action, info)) {
            return 1;
        }
        rpmb_report(info);
        if (action == MX_RPMB_ARM) {
            putln("Registration uses the stock-log profile; EXT_CSD region bytes remain assumed.");
        }
        return info.result == MX_RPMB_OK ? 0 : 1;
    }
    uint64_t cpu = 0;
    if (!parse_hex(argv[3], &cpu) || cpu == 0 || cpu >= MX_HOST_MAX_CPUS ||
        cpu == static_cast<uint64_t>(sys_thread_get_cpu())) {
        putln("Choose an online nonzero CPU different from the shell CPU (hex).");
        return 1;
    }
    MxDiagSystem system{};
    if (sys_debug_read(MX_DIAG_SYSTEM, 0, &system, sizeof(system)) != sizeof(system) ||
        system.version != MX_DIAG_VERSION || !(system.online_mask & (uint64_t{1} << cpu))) {
        putln("Selected worker CPU is unavailable.");
        return 1;
    }
    if (!rpmb_read(MX_RPMB_STATUS, info)) {
        return 1;
    }
    if (!info.armed || !info.registered || info.registering ||
        (serve && (!info.storage_enabled || info.provision_pending || info.done_inflight ||
                   (info.done_returned && info.done_result))) ||
        (counter && (info.trigger_started || info.events)) ||
        (complete && (info.done_inflight || info.events - info.completed_events != 1 ||
                      (info.done_started &&
                       (!info.storage_enabled || !info.done_returned || info.done_result))))) {
        rpmb_report(info);
        putln("Counter needs an idle registration; completion needs exactly one fresh SGI 5 and no "
              "active or failed FF06.");
        return 1;
    }
    const unsigned index = counter ? 0 : 1;
    for (const auto& worker : rpmb_workers) {
        if (__atomic_load_n(&worker.state, __ATOMIC_ACQUIRE) == 1 && worker.cpu == cpu) {
            putln("That CPU already has an outstanding probe worker.");
            return 1;
        }
    }
    auto& worker = rpmb_workers[index];
    if (__atomic_load_n(&worker.state, __ATOMIC_ACQUIRE) == 1) {
        putln("This probe worker has not returned.");
        return 1;
    }
    worker.cpu = cpu;
    worker.rc = 0;
    worker.cancel = 0;
    worker.continuous = serve;
    __atomic_store_n(&worker.state, 1u, __ATOMIC_RELEASE);
    const auto thread =
        sys_thread_create(0, reinterpret_cast<uint64_t>(&rpmb_worker), index, MX_THREAD_SUSPENDED);
    if (thread <= 0) {
        __atomic_store_n(&worker.state, 0u, __ATOMIC_RELEASE);
        error("RPMB worker create failed", thread);
        return 1;
    }
    if (sys_thread_set_affinity(static_cast<CapHandle>(thread), uint64_t{1} << cpu)) {
        // Retire the suspended helper without executing its firmware operation.
        __atomic_store_n(&worker.cancel, 1u, __ATOMIC_RELEASE);
        sys_thread_resume(static_cast<CapHandle>(thread));
        putln("RPMB worker affinity failed.");
        return 1;
    }
    if (sys_thread_resume(static_cast<CapHandle>(thread))) {
        putln("RPMB worker resume failed.");
        return 1;
    }
    putln(serve ? "RPMB service queued; it will complete successive SGI 5 requests. Use stop, then "
                  "release when idle."
          : counter ? "Counter trigger queued; use status to inspect SGI 5 and firmware return."
                    : "Storage completion queued; use status to inspect FF06 return.");
    return 0;
}

int el3_bl31read() {
    MxDiagEl3Bl31 probe{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 4, &probe, sizeof(probe));
    if (rc != sizeof(probe)) {
        error("EL3 BL31 read probe unavailable", rc);
        return 1;
    }
    hex("el3b.control_addr", probe.control_addr);
    hex("el3b.control_value", probe.control_value);
    hex("el3b.bl31_addr", probe.bl31_addr);
    decimal("el3b.bl31_read_ok", probe.bl31_read_ok);
    for (uint32_t i = 0; i < 4; ++i) {
        line("el3b.bl31_word%u=0x%llx", i, static_cast<unsigned long long>(probe.bl31_words[i]));
    }
    hex("el3b.teeos_addr", probe.teeos_addr);
    decimal("el3b.teeos_read_ok", probe.teeos_read_ok);
    for (uint32_t i = 0; i < 2; ++i) {
        line("el3b.teeos_word%u=0x%llx", i, static_cast<unsigned long long>(probe.teeos_words[i]));
    }
    decimal("el3b.crumb_stage", probe.crumb_stage);
    line("el3b.ticks_delta=%llu",
         static_cast<unsigned long long>(probe.tick_after - probe.tick_before));
    putln("EL3 BL31 probe: reads only (window control, BL31 0x35E00000, TEEOS 0x36208000).");
    return 0;
}

int el3_gv() {
    MxDiagEl3Gv gv{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 5, &gv, sizeof(gv));
    if (rc != sizeof(gv)) {
        error("EL3 gv probe unavailable", rc);
        return 1;
    }
    decimal("el3g.aborted", gv.aborted);
    hex("el3g.replay_x1", gv.replay_x1);
    hex("el3g.replay_ret", gv.replay_ret);
    hex("el3g.oracle_ret", gv.oracle_ret);
    hex("el3g.oracle_expected", gv.oracle_expected);
    decimal("el3g.oracle_match", gv.oracle_match);
    hex("el3g.bl31_ret", gv.bl31_ret);
    decimal("el3g.crumb_stage", gv.crumb_stage);
    line("el3g.ticks_delta=%llu", static_cast<unsigned long long>(gv.tick_after - gv.tick_before));
    putln("EL3 gv probe: 0xC500AA01 idx0 replay + oracle; BL31 read only on oracle match.");
    return 0;
}

int el3_aa(int argc, char** argv) {
    if (argc != 4) {
        putln("usage: diag el3-aa <idx 0-31> <x1 hex> <x2 hex>");
        return 1;
    }
    uint64_t idx = 0, x1 = 0, x2 = 0;
    if (!parse_hex(argv[1], &idx) || !parse_hex(argv[2], &x1) || !parse_hex(argv[3], &x2)) {
        putln("diag el3-aa: bad numbers");
        return 1;
    }
    if (idx > 31) {
        putln("diag el3-aa: idx out of range");
        return 1;
    }
    MxDiagEl3Aa aa{};
    aa.idx = idx;
    aa.x1 = x1;
    aa.x2 = x2;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 6, &aa, sizeof(aa));
    if (rc != sizeof(aa)) {
        error("EL3 aa probe unavailable", rc);
        return 1;
    }
    decimal("el3a.aborted", aa.aborted);
    line("el3a.idx=%llu", static_cast<unsigned long long>(aa.idx));
    hex("el3a.x1", aa.x1);
    hex("el3a.x2", aa.x2);
    hex("el3a.cookie", aa.cookie);
    hex("el3a.r0", aa.r0);
    hex("el3a.r1", aa.r1);
    hex("el3a.r2", aa.r2);
    hex("el3a.r3", aa.r3);
    decimal("el3a.crumb_stage", aa.crumb_stage);
    putln("EL3 aa probe: single 0xC500AA01 call; raw returns.");
    return 0;
}

static void print_chunk_line(uint64_t off, const uint8_t* d) {
    char buf[16 * 11 + 40];
    int n =
        libk::snprintf(buf, sizeof(buf), "el3s.chunk=0x%llx", static_cast<unsigned long long>(off));
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t v = static_cast<uint32_t>(d[i * 4]) |
                           (static_cast<uint32_t>(d[i * 4 + 1]) << 8) |
                           (static_cast<uint32_t>(d[i * 4 + 2]) << 16) |
                           (static_cast<uint32_t>(d[i * 4 + 3]) << 24);
        n += libk::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), " %08x", v);
    }
    putln(buf);
}

int el3_share(int argc, char** argv) {
    uint64_t off = 0;
    if (argc != 2 || !parse_hex(argv[1], &off)) {
        putln("usage: diag el3-share <offset hex>");
        return 1;
    }
    MxDiagEl3Share sh{};
    sh.offset = off;
    sh.len = 64;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 7, &sh, sizeof(sh));
    if (rc != sizeof(sh)) {
        error("EL3 share read unavailable", rc);
        return 1;
    }
    decimal("el3s.aborted", sh.aborted);
    print_chunk_line(sh.offset, sh.data);
    return 0;
}

int el3_sharedump() {
    uint32_t nonzero = 0;
    for (uint64_t off = 0; off < 0x8000; off += 64) {
        MxDiagEl3Share sh{};
        sh.offset = off;
        sh.len = 64;
        const auto rc = sys_debug_read(MX_DIAG_EL3, 7, &sh, sizeof(sh));
        if (rc != sizeof(sh)) {
            error("share dump unavailable", rc);
            return 1;
        }
        bool nz = false;
        for (uint32_t i = 0; i < 64; ++i) {
            if (sh.data[i] != 0) {
                nz = true;
                break;
            }
        }
        if (nz) {
            ++nonzero;
            print_chunk_line(off, sh.data);
        }
    }
    line("el3s.dump_nonzero_chunks=%u (of 512, 64B each)", nonzero);
    putln("EL3 share dump: first 32 KiB of the shared region; read only.");
    return 0;
}

int el3_ra(int argc, char** argv) {
    uint64_t tgt = 0;
    if (argc != 2 || !parse_hex(argv[1], &tgt)) {
        putln("usage: diag el3-ra <addr hex>");
        return 1;
    }
    MxDiagEl3Ra ra{};
    ra.target = tgt;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 8, &ra, sizeof(ra));
    if (rc != sizeof(ra)) {
        error("EL3 ra probe unavailable", rc);
        return 1;
    }
    decimal("el3ra.aborted", ra.aborted);
    hex("el3ra.target", ra.target);
    hex("el3ra.attempt_r0", ra.attempt_r0);
    hex("el3ra.attempt_r1", ra.attempt_r1);
    hex("el3ra.attempt_r2", ra.attempt_r2);
    hex("el3ra.attempt_r3", ra.attempt_r3);
    hex("el3ra.restore_r0", ra.restore_r0);
    hex("el3ra.restore_r1", ra.restore_r1);
    hex("el3ra.restore_r2", ra.restore_r2);
    hex("el3ra.restore_r3", ra.restore_r3);
    putln("EL3 ra probe: wrong-address registration attempt + restore.");
    return 0;
}

int el3_nn(int argc, char** argv) {
    uint64_t x1 = 0, x2 = 0, x3 = 0;
    if (argc != 4 || !parse_hex(argv[1], &x1) || !parse_hex(argv[2], &x2) ||
        !parse_hex(argv[3], &x3)) {
        putln("usage: diag el3-nn <x1> <x2> <x3> (hex)");
        return 1;
    }
    MxDiagEl3Nn nn{};
    nn.x1 = x1;
    nn.x2 = x2;
    nn.x3 = x3;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 9, &nn, sizeof(nn));
    if (rc != sizeof(nn)) {
        error("EL3 nn probe unavailable", rc);
        return 1;
    }
    line("el3n.x1=0x%llx r0=0x%llx r1=0x%llx r2=0x%llx r3=0x%llx",
         static_cast<unsigned long long>(nn.x1), static_cast<unsigned long long>(nn.r0),
         static_cast<unsigned long long>(nn.r1), static_cast<unsigned long long>(nn.r2),
         static_cast<unsigned long long>(nn.r3));
    putln("EL3 nn probe: single 0xC5009900 call.");
    return 0;
}

int el3_cc(int argc, char** argv) {
    uint64_t x1 = 0;
    if (argc != 2 || !parse_hex(argv[1], &x1)) {
        putln("usage: diag el3-cc <x1 hex>");
        return 1;
    }
    MxDiagEl3Cc cc{};
    cc.x1 = x1;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 11, &cc, sizeof(cc));
    if (rc != sizeof(cc)) {
        error("EL3 cc probe unavailable", rc);
        return 1;
    }
    line("el3c.x1=0x%llx r0=0x%llx r1=0x%llx r2=0x%llx r3=0x%llx crumb=0x%llx",
         static_cast<unsigned long long>(cc.x1), static_cast<unsigned long long>(cc.r0),
         static_cast<unsigned long long>(cc.r1), static_cast<unsigned long long>(cc.r2),
         static_cast<unsigned long long>(cc.r3), static_cast<unsigned long long>(cc.crumb_stage));
    putln("EL3 cc probe: single 0xC500CC00 call.");
    return 0;
}

int el3_cc2(int argc, char** argv) {
    uint64_t a = 0, b = 0;
    if (argc != 3 || !parse_hex(argv[1], &a) || !parse_hex(argv[2], &b)) {
        putln("usage: diag el3-cc2 <a> <b> (hex)");
        return 1;
    }
    MxDiagEl3Cc2 cc{};
    cc.a = a;
    cc.b = b;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 12, &cc, sizeof(cc));
    if (rc != sizeof(cc)) {
        error("EL3 cc2 probe unavailable", rc);
        return 1;
    }
    line("el3c2.a=0x%llx ra=0x%llx was=0x%llx/%llx", static_cast<unsigned long long>(cc.a),
         static_cast<unsigned long long>(cc.ra0), static_cast<unsigned long long>(cc.ra1),
         static_cast<unsigned long long>(cc.ra2));
    line("el3c2.b=0x%llx rb=0x%llx was=0x%llx/%llx crumb=0x%llx",
         static_cast<unsigned long long>(cc.b), static_cast<unsigned long long>(cc.rb0),
         static_cast<unsigned long long>(cc.rb1), static_cast<unsigned long long>(cc.rb2),
         static_cast<unsigned long long>(cc.crumb_stage));
    putln("EL3 cc2 probe: twin 0xC500CC00 calls (zero-gap).");
    return 0;
}

int el3_aasweep() {
    MxDiagEl3AaSweep sw{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 13, &sw, sizeof(sw));
    if (rc != sizeof(sw)) {
        error("EL3 aasweep unavailable", rc);
        return 1;
    }
    for (uint32_t i = 0; i < 32; ++i) {
        line("el3as.idx%02x=0x%llx", i, static_cast<unsigned long long>(sw.rows[i]));
    }
    line("el3as.k7=0x%llx", static_cast<unsigned long long>(sw.rows[32]));
    line("el3as.ka=0x%llx", static_cast<unsigned long long>(sw.rows[33]));
    line("el3as.ke=0x%llx", static_cast<unsigned long long>(sw.rows[34]));
    line("el3as.kf=0x%llx", static_cast<unsigned long long>(sw.rows[35]));
    decimal("el3as.crumb_stage", sw.crumb_stage);
    putln("EL3 aasweep: 0..31 plain + kernel forms (7/a/e/f).");
    return 0;
}

int el3_socp(int argc, char** argv) {
    if (argc != 8) {
        putln("usage: diag el3-socp <mode 1-4> <d0> <d1> <d2> <d3> <p1> <p2> (hex)");
        return 1;
    }
    MxDiagEl3Socp sc{};
    if (!parse_hex(argv[1], &sc.mode) || sc.mode < 1 || sc.mode > 4) {
        putln("bad mode");
        return 1;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (!parse_hex(argv[2 + i], &sc.d[i])) {
            putln("bad d");
            return 1;
        }
    }
    if (!parse_hex(argv[6], &sc.p1) || !parse_hex(argv[7], &sc.p2)) {
        putln("bad p");
        return 1;
    }
    const auto rc = sys_debug_read(MX_DIAG_EL3, 14, &sc, sizeof(sc));
    if (rc != sizeof(sc)) {
        error("EL3 socp probe unavailable", rc);
        return 1;
    }
    line("el3sp.d=0x%llx/0x%llx/0x%llx/0x%llx p1=0x%llx p2=0x%llx",
         static_cast<unsigned long long>(sc.d[0]), static_cast<unsigned long long>(sc.d[1]),
         static_cast<unsigned long long>(sc.d[2]), static_cast<unsigned long long>(sc.d[3]),
         static_cast<unsigned long long>(sc.p1), static_cast<unsigned long long>(sc.p2));
    line("el3sp.r8=0x%llx/0x%llx/0x%llx/0x%llx", static_cast<unsigned long long>(sc.r8[0]),
         static_cast<unsigned long long>(sc.r8[1]), static_cast<unsigned long long>(sc.r8[2]),
         static_cast<unsigned long long>(sc.r8[3]));
    line("el3sp.rA=0x%llx/0x%llx/0x%llx/0x%llx", static_cast<unsigned long long>(sc.rA[0]),
         static_cast<unsigned long long>(sc.rA[1]), static_cast<unsigned long long>(sc.rA[2]),
         static_cast<unsigned long long>(sc.rA[3]));
    for (uint32_t i = 0; i < 16; i += 4) {
        line("el3sp.rb%u=0x%llx 0x%llx 0x%llx 0x%llx", i,
             static_cast<unsigned long long>(sc.readback[i]),
             static_cast<unsigned long long>(sc.readback[i + 1]),
             static_cast<unsigned long long>(sc.readback[i + 2]),
             static_cast<unsigned long long>(sc.readback[i + 3]));
    }
    decimal("el3sp.crumb_stage", sc.crumb_stage);
    putln("EL3 socp probe: desc write + idx8 + idxA + readback.");
    return 0;
}

int el3_put(int argc, char** argv) {
    uint64_t off = 0, val = 0;
    if (argc != 3 || !parse_hex(argv[1], &off) || !parse_hex(argv[2], &val)) {
        putln("usage: diag el3-put <off> <val> (hex)");
        return 1;
    }
    MxDiagEl3Put pt{};
    pt.off = off;
    pt.val = val;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 15, &pt, sizeof(pt));
    if (rc != sizeof(pt)) {
        error("EL3 put unavailable", rc);
        return 1;
    }
    decimal("el3p.aborted", pt.aborted);
    hex("el3p.off", pt.off);
    hex("el3p.val", pt.val);
    hex("el3p.readback", pt.readback);
    decimal("el3p.crumb_stage", pt.crumb_stage);
    putln("EL3 put: single 8-byte write to share+off.");
    return 0;
}

int el3_poke(int argc, char** argv) {
    uint64_t cb = 0;
    if (argc != 2 || !parse_hex(argv[1], &cb)) {
        putln("usage: diag el3-poke <cb hex>");
        return 1;
    }
    MxDiagEl3Poke pk{};
    pk.cb = cb;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 16, &pk, sizeof(pk));
    if (rc != sizeof(pk)) {
        error("EL3 poke unavailable", rc);
        return 1;
    }
    decimal("el3pk.aborted", pk.aborted);
    hex("el3pk.cb", pk.cb);
    hex("el3pk.registered", pk.registered);
    hex("el3pk.slot_pre", pk.slot_pre);
    hex("el3pk.slot_written", pk.slot_written);
    hex("el3pk.status_set", pk.status_set);
    hex("el3pk.call_x0", pk.call_x0);
    hex("el3pk.call_x1", pk.call_x1);
    hex("el3pk.call_x2", pk.call_x2);
    hex("el3pk.call_x3", pk.call_x3);
    hex("el3pk.slot_post", pk.slot_post);
    hex("el3pk.status_post", pk.status_post);
    line("el3pk.ticks=%llu", static_cast<unsigned long long>(pk.tick_after - pk.tick_before));
    decimal("el3pk.crumb_stage", pk.crumb_stage);
    putln("EL3 poke: FF04 register then FF06 with caller-chosen callback slot.");
    return 0;
}

int el3_svc(int argc, char** argv) {
    uint64_t fid = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0;
    if (argc < 5 || argc > 9 || !parse_hex(argv[1], &fid) || !parse_hex(argv[2], &x1) ||
        !parse_hex(argv[3], &x2) || !parse_hex(argv[4], &x3) ||
        (argc > 5 && !parse_hex(argv[5], &x4)) || (argc > 6 && !parse_hex(argv[6], &x5)) ||
        (argc > 7 && !parse_hex(argv[7], &x6)) || (argc > 8 && !parse_hex(argv[8], &x7))) {
        putln("usage: diag el3-svc <fid> <x1> <x2> <x3> [x4 x5 x6 x7] (hex)");
        return 1;
    }
    MxDiagEl3Svc sv{};
    sv.fid = fid;
    sv.x1 = x1;
    sv.x2 = x2;
    sv.x3 = x3;
    sv.x4 = x4;
    sv.x5 = x5;
    sv.x6 = x6;
    sv.x7 = x7;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 17, &sv, sizeof(sv));
    if (rc != sizeof(sv)) {
        error("EL3 svc unavailable", rc);
        return 1;
    }
    decimal("el3sv.aborted", sv.aborted);
    hex("el3sv.fid", sv.fid);
    hex("el3sv.x1", sv.x1);
    hex("el3sv.x2", sv.x2);
    hex("el3sv.x3", sv.x3);
    hex("el3sv.r0", sv.r0);
    hex("el3sv.r1", sv.r1);
    hex("el3sv.r2", sv.r2);
    hex("el3sv.r3", sv.r3);
    decimal("el3sv.crumb_stage", sv.crumb_stage);
    putln("EL3 svc: single arbitrary secure call with raw returns.");
    return 0;
}

int el3_win(int argc, char** argv) {
    uint64_t off = 0;
    if (argc != 2 || !parse_hex(argv[1], &off)) {
        putln("usage: diag el3-win <off> (hex)");
        return 1;
    }
    MxDiagEl3Win wn{};
    wn.off = off;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 18, &wn, sizeof(wn));
    if (rc != sizeof(wn)) {
        error("EL3 win read unavailable", rc);
        return 1;
    }
    decimal("el3w.aborted", wn.aborted);
    hex("el3w.off", wn.off);
    for (uint32_t i = 0; i < 8; i += 4) {
        line("el3w.w%u=0x%llx 0x%llx 0x%llx 0x%llx", i,
             static_cast<unsigned long long>(wn.words[i]),
             static_cast<unsigned long long>(wn.words[i + 1]),
             static_cast<unsigned long long>(wn.words[i + 2]),
             static_cast<unsigned long long>(wn.words[i + 3]));
    }
    decimal("el3w.crumb_stage", wn.crumb_stage);
    putln("EL3 win: 64-byte read at 0x34A29000+off.");
    return 0;
}

int el3_wput(int argc, char** argv) {
    uint64_t off = 0, val = 0;
    if (argc != 3 || !parse_hex(argv[1], &off) || !parse_hex(argv[2], &val)) {
        putln("usage: diag el3-wput <off> <val> (hex)");
        return 1;
    }
    MxDiagEl3Wput wp{};
    wp.off = off;
    wp.val = val;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 19, &wp, sizeof(wp));
    if (rc != sizeof(wp)) {
        error("EL3 wput unavailable", rc);
        return 1;
    }
    decimal("el3wp.aborted", wp.aborted);
    hex("el3wp.off", wp.off);
    hex("el3wp.val", wp.val);
    hex("el3wp.readback", wp.readback);
    decimal("el3wp.crumb_stage", wp.crumb_stage);
    putln("EL3 wput: single 8-byte write to window+off.");
    return 0;
}

int el3_rmap(int argc, char** argv) {
    uint64_t addr = 0, size = 0;
    if ((argc != 2 && argc != 3) || !parse_hex(argv[1], &addr)) {
        putln("usage: diag el3-rmap <addr> [size] (hex; size<=0x20000)");
        return 1;
    }
    if (argc == 3 && !parse_hex(argv[2], &size)) {
        putln("usage: diag el3-rmap <addr> [size] (hex; size<=0x20000)");
        return 1;
    }
    MxDiagEl3Rmap rm{};
    rm.addr = addr;
    rm.size = size;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 24, &rm, sizeof(rm));
    if (rc != sizeof(rm)) {
        error("EL3 rmap unavailable", rc);
        return 1;
    }
    decimal("el3rm.aborted", rm.aborted);
    hex("el3rm.addr", rm.addr);
    hex("el3rm.size", rm.size);
    decimal("el3rm.mapped_global", rm.mapped_global);
    decimal("el3rm.mapped_current", rm.mapped_current);
    decimal("el3rm.ok", rm.ok);
    hex("el3rm.value", rm.value);
    hex("el3rm.esr", rm.esr);
    hex("el3rm.far", rm.far);
    decimal("el3rm.faults", rm.faults);
    putln("EL3 rmap: map window (Device, RO) then one fault-tolerant u64 read.");
    return 0;
}

// ---- sacrificial secure-call runner ----------------------------------------
// Fires a diag EL3 op from a thread pinned to a chosen CPU. If the secure call
// wedges, only that CPU parks in EL3; msh (and the rest of the system) keeps
// running, and the lock-free rawin/rawout commands can watch the window state.
struct SacState {
    volatile uint32_t state; // 0 idle, 1 busy, 2 done
    uint64_t cpu, fid, x1, x2, x3, x4, x5, x6, x7;
    int64_t rc, worker_cpu;
    uint64_t r0, r1, r2, r3;
    uint32_t crumb;
};
SacState g_sac{};
CapHandle g_sac_thread = kNullCap;

void sac_worker(uint64_t arg) {
    (void)arg;
    g_sac.worker_cpu = sys_thread_get_cpu();
    __atomic_store_n(&g_sac.state, 1u, __ATOMIC_RELEASE);
    MxDiagEl3Svc sv{};
    sv.fid = g_sac.fid;
    sv.x1 = g_sac.x1;
    sv.x2 = g_sac.x2;
    sv.x3 = g_sac.x3;
    sv.x4 = g_sac.x4;
    sv.x5 = g_sac.x5;
    sv.x6 = g_sac.x6;
    sv.x7 = g_sac.x7;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 25, &sv, sizeof(sv));
    g_sac.rc = rc;
    if (rc == static_cast<int64_t>(sizeof(sv))) {
        g_sac.r0 = sv.r0;
        g_sac.r1 = sv.r1;
        g_sac.r2 = sv.r2;
        g_sac.r3 = sv.r3;
        g_sac.crumb = static_cast<uint32_t>(sv.crumb_stage);
    }
    __atomic_store_n(&g_sac.state, 2u, __ATOMIC_RELEASE);
    sys_thread_exit(0);
    for (;;) {
        sys_yield();
    }
}

int sac_fire(int argc, char** argv) {
    uint64_t cpu = 0, fid = 0, x1 = 0, x2 = 0, x3 = 0, x4 = 0, x5 = 0, x6 = 0, x7 = 0;
    if (argc < 6 || argc > 10 || !parse_hex(argv[1], &cpu) || !parse_hex(argv[2], &fid) ||
        !parse_hex(argv[3], &x1) || !parse_hex(argv[4], &x2) || !parse_hex(argv[5], &x3) ||
        (argc > 6 && !parse_hex(argv[6], &x4)) || (argc > 7 && !parse_hex(argv[7], &x5)) ||
        (argc > 8 && !parse_hex(argv[8], &x6)) || (argc > 9 && !parse_hex(argv[9], &x7))) {
        putln("usage: sac <cpu> <fid> <x1> <x2> <x3> [x4 x5 x6 x7] (hex)");
        return 1;
    }
    if (cpu == 0 || cpu > 31) {
        putln("sac: choose a nonzero sacrificial cpu");
        return 1;
    }
    if (__atomic_load_n(&g_sac.state, __ATOMIC_ACQUIRE) == 1u) {
        putln("sac: already busy");
        return 1;
    }
    g_sac.cpu = cpu;
    g_sac.fid = fid;
    g_sac.x1 = x1;
    g_sac.x2 = x2;
    g_sac.x3 = x3;
    g_sac.x4 = x4;
    g_sac.x5 = x5;
    g_sac.x6 = x6;
    g_sac.x7 = x7;
    g_sac.rc = 0;
    g_sac.worker_cpu = -1;
    g_sac.crumb = 0;
    __atomic_store_n(&g_sac.state, 0u, __ATOMIC_RELEASE);
    const auto t =
        sys_thread_create(0, reinterpret_cast<uint64_t>(&sac_worker), 0, MX_THREAD_SUSPENDED);
    if (t <= 0) {
        error("sac thread create", t);
        return 1;
    }
    g_sac_thread = static_cast<CapHandle>(t);
    if (sys_thread_set_affinity(g_sac_thread, uint64_t{1} << cpu)) {
        putln("sac: affinity failed (cpu offline?)");
        return 1;
    }
    if (sys_thread_resume(g_sac_thread)) {
        putln("sac: resume failed");
        return 1;
    }
    line("sac.fired cpu=%llu fid=0x%llx", static_cast<unsigned long long>(cpu),
         static_cast<unsigned long long>(fid));
    return 0;
}

int sac_stat(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const uint32_t st = __atomic_load_n(&g_sac.state, __ATOMIC_ACQUIRE);
    decimal("sac.state", st);
    hex("sac.cpu", g_sac.cpu);
    hex("sac.fid", g_sac.fid);
    line("sac.args=0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx",
         static_cast<unsigned long long>(g_sac.x1), static_cast<unsigned long long>(g_sac.x2),
         static_cast<unsigned long long>(g_sac.x3), static_cast<unsigned long long>(g_sac.x4),
         static_cast<unsigned long long>(g_sac.x5), static_cast<unsigned long long>(g_sac.x6),
         static_cast<unsigned long long>(g_sac.x7));
    line("sac.worker_cpu=%lld", static_cast<long long>(g_sac.worker_cpu));
    line("sac.shell_cpu=%lld", static_cast<long long>(sys_thread_get_cpu()));
    if (st == 2u) {
        line("sac.rc=%lld", static_cast<long long>(g_sac.rc));
        hex("sac.r0", g_sac.r0);
        hex("sac.r1", g_sac.r1);
        hex("sac.r2", g_sac.r2);
        hex("sac.r3", g_sac.r3);
        decimal("sac.crumb", g_sac.crumb);
    }
    putln("sac: state 1 = the secure call is still inside EL3 (hang held).");
    return 0;
}

int el3_rawin(int argc, char** argv) {
    uint64_t off = 0;
    if (argc != 2 || !parse_hex(argv[1], &off)) {
        putln("usage: diag el3-rawin <off> (hex) - lock-free window read");
        return 1;
    }
    MxDiagEl3Win wn{};
    wn.off = off;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 26, &wn, sizeof(wn));
    if (rc != static_cast<int64_t>(sizeof(wn))) {
        error("EL3 rawin unavailable", rc);
        return 1;
    }
    decimal("el3ri.aborted", wn.aborted);
    hex("el3ri.off", wn.off);
    for (uint32_t i = 0; i < 8; i += 4) {
        line("el3ri.w%u=0x%llx 0x%llx 0x%llx 0x%llx", i,
             static_cast<unsigned long long>(wn.words[i]),
             static_cast<unsigned long long>(wn.words[i + 1]),
             static_cast<unsigned long long>(wn.words[i + 2]),
             static_cast<unsigned long long>(wn.words[i + 3]));
    }
    decimal("el3ri.crumb", wn.crumb_stage);
    putln("EL3 rawin: lock-free 64-byte read at 0x34A29000+off.");
    return 0;
}

int el3_rawout(int argc, char** argv) {
    uint64_t off = 0, val = 0;
    if (argc != 3 || !parse_hex(argv[1], &off) || !parse_hex(argv[2], &val)) {
        putln("usage: diag el3-rawout <off> <val> (hex) - lock-free window write");
        return 1;
    }
    MxDiagEl3Wput wp{};
    wp.off = off;
    wp.val = val;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 27, &wp, sizeof(wp));
    if (rc != static_cast<int64_t>(sizeof(wp))) {
        error("EL3 rawout unavailable", rc);
        return 1;
    }
    decimal("el3ro.aborted", wp.aborted);
    hex("el3ro.off", wp.off);
    hex("el3ro.val", wp.val);
    hex("el3ro.readback", wp.readback);
    decimal("el3ro.crumb", wp.crumb_stage);
    putln("EL3 rawout: lock-free 8-byte write to window+off.");
    return 0;
}

int el3_wmemset(int argc, char** argv) {
    uint64_t off = 0, len = 0, val = 0;
    if ((argc != 3 && argc != 4) || !parse_hex(argv[1], &off) || !parse_hex(argv[2], &len) ||
        (argc == 4 && !parse_hex(argv[3], &val))) {
        putln("usage: diag el3-wmemset <off> <len> [val] (hex) - lock-free window fill");
        return 1;
    }
    MxDiagEl3Wmemset wm{};
    wm.off = off;
    wm.len = len;
    wm.val = val;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 28, &wm, sizeof(wm));
    if (rc != static_cast<int64_t>(sizeof(wm))) {
        error("EL3 wmemset unavailable", rc);
        return 1;
    }
    decimal("el3wm.aborted", wm.aborted);
    hex("el3wm.off", wm.off);
    hex("el3wm.len", wm.len);
    hex("el3wm.val", wm.val);
    decimal("el3wm.crumb", wm.crumb_stage);
    putln("EL3 wmemset: lock-free fill of window[off..off+len).");
    return 0;
}

int el3_ipi(int argc, char** argv) {
    uint64_t cpu = 0;
    if (argc != 2 || !parse_hex(argv[1], &cpu)) {
        putln("usage: diag el3-ipi <cpu> (hex)");
        return 1;
    }
    const auto rc = sys_debug_read(MX_DIAG_EL3, 29, &cpu, sizeof(cpu));
    if (rc != static_cast<int64_t>(sizeof(cpu))) {
        error("EL3 ipi unavailable", rc);
        return 1;
    }
    putln("EL3 ipi: wake IPI sent.");
    return 0;
}

int el3_rprobe(int argc, char** argv) {
    uint64_t addr = 0;
    if (argc != 2 || !parse_hex(argv[1], &addr)) {
        putln("usage: diag el3-rprobe <addr> (hex)");
        return 1;
    }
    MxDiagEl3Rprobe rp{};
    rp.addr = addr;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 22, &rp, sizeof(rp));
    if (rc != sizeof(rp)) {
        error("EL3 rprobe unavailable", rc);
        return 1;
    }
    decimal("el3rp.ok", rp.ok);
    hex("el3rp.addr", rp.addr);
    hex("el3rp.value", rp.value);
    hex("el3rp.esr", rp.esr);
    hex("el3rp.far", rp.far);
    hex("el3rp.elr", rp.elr);
    decimal("el3rp.faults", rp.faults);
    putln("EL3 rprobe: fault-tolerant u64 read; ok=1 means value valid.");
    return 0;
}

int el3_rscan(int argc, char** argv) {
    uint64_t base = 0, len = 0;
    if (argc != 3 || !parse_hex(argv[1], &base) || !parse_hex(argv[2], &len)) {
        putln("usage: diag el3-rscan <base> <len> (hex)");
        return 1;
    }
    MxDiagEl3Rscan rs{};
    rs.base = base;
    rs.length = len;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 23, &rs, sizeof(rs));
    if (rc != sizeof(rs)) {
        error("EL3 rscan unavailable", rc);
        return 1;
    }
    decimal("el3rs.aborted", rs.aborted);
    hex("el3rs.base", rs.base);
    decimal("el3rs.readable", rs.readable);
    decimal("el3rs.faults", rs.faults);
    hex("el3rs.esr_last", rs.esr_last);
    decimal("el3rs.seg_count", rs.seg_count);
    for (uint32_t i = 0; i < rs.seg_count && i < 8; i++) {
        line("el3rs.seg%u=0x%llx-0x%llx", i, static_cast<unsigned long long>(rs.segments[i][0]),
             static_cast<unsigned long long>(rs.segments[i][1]));
    }
    decimal("el3rs.first_count", rs.first_count);
    for (uint32_t i = 0; i < rs.first_count && i < 8; i++) {
        line("el3rs.first%u=0x%llx:0x%llx", i,
             static_cast<unsigned long long>(rs.first_values[i][0]),
             static_cast<unsigned long long>(rs.first_values[i][1]));
    }
    putln("EL3 rscan: fault-tolerant u64 sweep; faults are skipped, not fatal.");
    return 0;
}

int el3_scan(int argc, char** argv) {
    uint64_t base = 0, len = 0, lo = 0, hi = 0;
    if (argc != 5 || !parse_hex(argv[1], &base) || !parse_hex(argv[2], &len) ||
        !parse_hex(argv[3], &lo) || !parse_hex(argv[4], &hi)) {
        putln("usage: diag el3-scan <base> <len> <lo> <hi> (hex)");
        return 1;
    }
    MxDiagEl3Scan sc{};
    sc.base = base;
    sc.len = len;
    sc.lo = lo;
    sc.hi = hi;
    const auto rc = sys_debug_read(MX_DIAG_EL3, 20, &sc, sizeof(sc));
    if (rc != sizeof(sc)) {
        error("EL3 scan unavailable", rc);
        return 1;
    }
    decimal("el3sc.aborted", sc.aborted);
    hex("el3sc.base", sc.base);
    decimal("el3sc.count", sc.count);
    for (uint32_t i = 0; i < sc.count && i < 20; i++) {
        line("el3sc.h%u=+0x%llx:0x%llx", i, static_cast<unsigned long long>(sc.hits[i][0]),
             static_cast<unsigned long long>(sc.hits[i][1]));
    }
    decimal("el3sc.crumb_stage", sc.crumb_stage);
    putln("EL3 scan: u64 values in [lo,hi] within region.");
    return 0;
}

int el3tool(int argc, char** argv) {
    if (argc < 2) {
        putln("usage: el3tool scan | dump <base> <len> | sweep | ff06 <x1> <x2> <x3>");
        return 1;
    }
    if (streq(argv[1], "scan")) {
        MxDiagEl3Scan2 sc{};
        struct {
            uint64_t b, l;
        } hot[8] = {
            {0x2264000, 0x1000},  {0x2272000, 0x4000},  {0x228c000, 0x1000}, {0x1f0c000, 0x1000},
            {0x34a21000, 0x8000}, {0x34a29000, 0x8000}, {0x2750000, 0x8000}, {0x1010000, 0x8000},
        };
        sc.nranges = 8;
        for (uint32_t i = 0; i < 8; i++) {
            sc.ranges[i][0] = hot[i].b;
            sc.ranges[i][1] = hot[i].l;
        }
        sc.lo = 0x35e00000ull;
        sc.hi = 0x37000000ull;
        const auto rc = sys_debug_read(MX_DIAG_EL3, 21, &sc, sizeof(sc));
        if (rc != sizeof(sc)) {
            error("el3tool scan unavailable", rc);
            return 1;
        }
        decimal("el3t.aborted", sc.aborted);
        decimal("el3t.count", sc.count);
        for (uint32_t i = 0; i < sc.count && i < 32; i++) {
            line("el3t.h%u=[r%u+0x%llx]=0x%llx", i, static_cast<unsigned>(sc.hits[i][0]),
                 static_cast<unsigned long long>(sc.hits[i][1]),
                 static_cast<unsigned long long>(sc.hits[i][2]));
        }
        decimal("el3t.crumb_stage", sc.crumb_stage);
        putln("el3tool scan: pointer hunt in hot ranges.");
        return 0;
    }
    if (streq(argv[1], "dump") && argc == 4) {
        uint64_t base = 0, len = 0;
        if (!parse_hex(argv[2], &base) || !parse_hex(argv[3], &len)) {
            putln("usage: el3tool dump <base> <len>");
            return 1;
        }
        MxDiagEl3Scan2 sc{};
        sc.nranges = 1;
        sc.ranges[0][0] = base;
        sc.ranges[0][1] = len;
        sc.lo = 1;
        sc.hi = 0xffffffffffffffffull;
        const auto rc = sys_debug_read(MX_DIAG_EL3, 21, &sc, sizeof(sc));
        if (rc != sizeof(sc)) {
            error("el3tool dump unavailable", rc);
            return 1;
        }
        decimal("el3t.aborted", sc.aborted);
        decimal("el3t.count", sc.count);
        for (uint32_t i = 0; i < sc.count && i < 32; i++) {
            line("el3t.d%u=+0x%llx:0x%llx", i, static_cast<unsigned long long>(sc.hits[i][1]),
                 static_cast<unsigned long long>(sc.hits[i][2]));
        }
        putln("el3tool dump: nonzero u64 dump.");
        return 0;
    }
    if (streq(argv[1], "sweep")) {
        static const uint64_t fids[] = {
            0xc7000000, 0xc7000001, 0xc7000002, 0xc7000003, 0xc7000004, 0xc7000007,
            0xc7000707, 0xc8000000, 0xc8000001, 0xc8000002, 0xc8000003, 0xc8000004,
            0xc8000007, 0xc8000808, 0x8500aa01, 0x87000000, 0x88000000,
        };
        for (unsigned i = 0; i < sizeof(fids) / sizeof(fids[0]); ++i) {
            MxDiagEl3Svc sv{};
            sv.fid = fids[i];
            const auto rc = sys_debug_read(MX_DIAG_EL3, 17, &sv, sizeof(sv));
            if (rc != sizeof(sv)) {
                line("el3t.s[%u]=unavail(%d)", i, static_cast<int>(rc));
                break;
            }
            line("el3t.s[%u]=0x%llx 0x%llx", i, static_cast<unsigned long long>(sv.r0),
                 static_cast<unsigned long long>(sv.r1));
        }
        putln("el3tool sweep: service fid probes done.");
        return 0;
    }
    if (streq(argv[1], "ff06") && argc == 5) {
        uint64_t x1 = 0, x2 = 0, x3 = 0;
        if (!parse_hex(argv[2], &x1) || !parse_hex(argv[3], &x2) || !parse_hex(argv[4], &x3)) {
            putln("usage: el3tool ff06 <x1> <x2> <x3>");
            return 1;
        }
        MxDiagEl3Svc sv{};
        sv.fid = 0xc600ff06ull;
        sv.x1 = x1;
        sv.x2 = x2;
        sv.x3 = x3;
        const auto rc = sys_debug_read(MX_DIAG_EL3, 17, &sv, sizeof(sv));
        if (rc != sizeof(sv)) {
            error("el3tool ff06 unavailable", rc);
            return 1;
        }
        line("el3t.ff06=0x%llx 0x%llx 0x%llx 0x%llx", static_cast<unsigned long long>(sv.r0),
             static_cast<unsigned long long>(sv.r1), static_cast<unsigned long long>(sv.r2),
             static_cast<unsigned long long>(sv.r3));
        decimal("el3t.crumb_stage", sv.crumb_stage);
        putln("el3tool ff06: done.");
        return 0;
    }
    putln("usage: el3tool scan | dump <base> <len> | sweep | ff06 <x1> <x2> <x3>");
    return 1;
}

int el3_teeos() {
    MxDiagEl3Teeos tt{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 10, &tt, sizeof(tt));
    if (rc != sizeof(tt)) {
        error("EL3 teeos probe unavailable", rc);
        return 1;
    }
    decimal("el3t.ok", tt.ok);
    hex("el3t.addr", tt.addr);
    hex("el3t.word0", tt.words[0]);
    hex("el3t.word1", tt.words[1]);
    decimal("el3t.crumb_stage", tt.crumb_stage);
    putln("EL3 teeos probe: single read attempt at 0x36208000.");
    return 0;
}

struct Counter {
    uint64_t id;
    const char* name;
};
constexpr Counter counters[] = {
    {DBG_SYSCALL_COUNT, "syscalls"},
    {DBG_IPC_SYSCALL_COUNT, "ipc.syscalls"},
    {DBG_CONTEXT_SWITCHES, "sched.switches"},
    {DBG_DEBUG_PUTS, "debug.puts"},
    {DBG_EL0_FAULTS, "el0.faults"},
    {DBG_DOORBELL_SLEEPS, "doorbell.sleeps"},
    {DBG_DOORBELL_WAKES, "doorbell.wakes"},
    {DBG_TIMER_TICKS, "timer.ticks"},
    {DBG_DOORBELL_WAIT_CALLS, "doorbell.waits"},
    {DBG_DOORBELL_WAIT_IMMED, "doorbell.immediate"},
    {DBG_NOTIFY_CALLS, "notify.calls"},
    {DBG_NOTIFY_NO_WAITER, "notify.no_waiter"},
    {DBG_NOTIFY_WOKE, "notify.woke"},
    {DBG_YIELD_COUNT, "sched.yields"},
    {DBG_BLOCK_SWITCHES, "sched.blocks"},
    {DBG_TIMER_PREEMPTS, "sched.preempts"},
    {DBG_EXIT_SWITCHES, "sched.exits"},
    {DBG_ONLINE_CPUS, "cpu.online_count"},
    {DBG_CPU0_CTX_SWITCHES, "cpu0.switches"},
    {DBG_CPU1_CTX_SWITCHES, "cpu1.switches"},
    {DBG_CPU0_TIMER_PREEMPTS, "cpu0.preempts"},
    {DBG_CPU1_TIMER_PREEMPTS, "cpu1.preempts"},
    {DBG_LOCAL_WAKES, "sched.local_wakes"},
    {DBG_REMOTE_WAKES, "sched.remote_wakes"},
    {DBG_IPI_SENT, "ipi.sent"},
    {DBG_IPI_HANDLED, "ipi.handled"},
    {DBG_PENDING_WAKE_DRAINS, "sched.wake_drains"},
    {DBG_DOORBELL_LOCAL_WAKES, "doorbell.local_wakes"},
    {DBG_DOORBELL_REMOTE_WAKES, "doorbell.remote_wakes"},
    {DBG_FAIRNESS_LAG, "sched.fairness_lag"},
    {DBG_PRINCIPAL_TRUSTED_RT, "sched.trusted_runtime"},
    {DBG_PRINCIPAL_SANDBOX_RT, "sched.sandbox_runtime"},
    {DBG_WAKE_DOORBELL, "sched.wake_doorbell"},
    {DBG_WAKE_REMOTE, "sched.wake_remote"},
    {DBG_WAKE_RESUME, "sched.wake_resume"},
    {DBG_IRQ_DELIVERED, "irq.delivered"},
    {DBG_IRQ_BOUND, "irq.bound"},
    {DBG_IRQ_SELFTEST, "irq.selftest"},
    {DBG_BOOT_CS, "boot.centiseconds"},
    {200, "objects.live"},
    {201, "memory.allocated_pages"},
    {202, "shared_arena.attached"},
    {203, "shared_arena.dma_registered"},
};
// All existing VM debug IDs 32..63, including assignment counters which the
// older libdango header does not yet name. Keep their wire order explicit.
constexpr const char* vm_counters[] = {
    "create_calls",
    "created",
    "destroyed",
    "memory_created",
    "memory_mapped",
    "memory_unmapped",
    "stage2_tables",
    "stage2_pages",
    "vcpu_created",
    "vcpu_configured",
    "vcpu_runs",
    "vcpu_notimpl",
    "channels",
    "errors",
    "host_el",
    "el2_available",
    "vhe_available",
    "guest_boot_stub",
    "guest_agent_start",
    "guest_linux_hello",
    "wfx_exits",
    "irq_exits",
    "unsupported_exits",
    "device_created",
    "device_active",
    "device_destroyed",
    "device_bar_mapped",
    "device_irq_routes",
    "device_irq_injected",
    "device_mediated_deny",
    "device_bme_enable",
    "device_quarantine",
};
static_assert(sizeof(vm_counters) / sizeof(vm_counters[0]) == 32);

int runtime_info() {
    int result = 0;
    putln("--- counters (live samples) ---");
    for (const auto& counter : counters) {
        const auto value = sys_debug_counter(counter.id);
        if (value < 0) {
            error(counter.name, value);
            result = 1;
        } else {
            decimal(counter.name, static_cast<uint64_t>(value));
        }
    }
    for (uint32_t i = 0; i < 32; ++i) {
        const auto value = sys_debug_counter(dango::VM_DEBUG_CREATE_CALLS + i);
        line("vm.%s=%lld", vm_counters[i], static_cast<long long>(value));
        if (value < 0) {
            result = 1;
        }
    }
    putln("--- shell endpoints ---");
    decimal("shell.terminal", g.have_terminal);
    decimal("shell.filesystem", g.have_fs);
    decimal("shell.service_control", g.have_svcctl);
    putln("--- boot resources ---");
    if (cmd_bootinfo(0, nullptr)) {
        result = 1;
    }
    if (cmd_cap(0, nullptr)) {
        result = 1;
    }
    if (cmd_ring(0, nullptr)) {
        result = 1;
    }
    if (cmd_doorbell(0, nullptr)) {
        result = 1;
    }
    putln("--- services ---");
    if (cmd_svc(0, nullptr)) {
        result = 1;
    }
    return result;
}

int boot_log(const MxDiagSystem& info) {
    putln("--- retained boot log ---");
    if (!info.boot_log_capacity) {
        putln("retained boot log unavailable on this board");
        return 0;
    }
    if (info.boot_log_size > info.boot_log_capacity) {
        putln("invalid retained log size");
        return 1;
    }
    decimal("boot_log.bytes", info.boot_log_size);
    decimal("boot_log.capacity", info.boot_log_capacity);
    if (info.boot_log_size == info.boot_log_capacity) {
        putln("boot log capacity reached; later records may be missing");
    }
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
            error("retained log read incomplete", count);
            return 1;
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
} // namespace

int cmd_ipc(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const SysPair counts = mx_ipc_policy_counts();
    line("ipc.budget=%u parks=%llu wakes=%llu", static_cast<unsigned>(mx_ipc_policy_budget()),
         static_cast<unsigned long long>(counts.value),
         static_cast<unsigned long long>(counts.detail));
    return 0;
}

namespace {
int gpu_iommu_report() {
    MxDiagGpuIommu info{};
    const auto rc = sys_debug_read(MX_DIAG_EL3, 31, &info, sizeof(info));
    if (rc != sizeof(info) || info.version != MX_DIAG_VERSION) {
        error("GPU IOMMU snapshot unavailable", rc);
        return 1;
    }
    hex("gpu.iommu.valid", info.valid);
    decimal("gpu.iommu.read_faults", info.faults);
    if (info.valid & 1) {
        hex("gpu.id", info.gpu_id);
    }
    if (info.valid & 2) {
        hex("gpu.mem_features", info.mem_features);
    }
    if (info.valid & 4) {
        hex("gpu.mmu_features", info.mmu_features);
        decimal("gpu.mmu.input_bits", info.mmu_features & 255);
        decimal("gpu.mmu.output_bits", (info.mmu_features >> 8) & 255);
    }
    if (info.valid & 8) {
        hex("gpu.as_present", info.as_present);
    }
    if (info.valid & 16) {
        hex("gpu.pctrl_ctrl8", info.pctrl_ctrl8);
        decimal("gpu.armid0", info.pctrl_ctrl8 & 31);
        decimal("gpu.awmid0", (info.pctrl_ctrl8 >> 5) & 31);
        decimal("gpu.armid1", (info.pctrl_ctrl8 >> 10) & 31);
        decimal("gpu.awmid1", (info.pctrl_ctrl8 >> 15) & 31);
    }
    if (info.valid & 32) {
        hex("gpu.pctrl_ctrl25", info.pctrl_ctrl25);
        decimal("gpu.smmu_sec_override", (info.pctrl_ctrl25 >> 11) & 1);
    }
    putln("Read-only snapshot; bus master IDs do not establish an external SMMU route.");
    return info.valid == 63 ? 0 : 1;
}
} // namespace

int cmd_diag(int argc, char** argv) {
    if (argc == 2 && streq(argv[1], "gpu-smmu")) {
        MxDiagSmmuId info{};
        const auto rc = sys_debug_read(MX_DIAG_EL3, 33, &info, sizeof(info));
        if (rc != sizeof(info) || info.version != MX_DIAG_VERSION) {
            error("SMMU identity snapshot unavailable", rc);
            return 1;
        }
        hex("smmu.candidate_base", info.base);
        hex("smmu.valid", info.valid);
        decimal("smmu.read_faults", info.faults);
        if (info.faults) {
            hex("smmu.first_esr", info.first_esr);
            hex("smmu.first_far", info.first_far);
        }
        const char* names[] = {"smmu.id0", "smmu.id1", "smmu.id2"};
        for (uint32_t i = 0; i < 3; ++i) {
            if (info.valid & (1u << i)) {
                hex(names[i], info.id[i]);
            }
        }
        putln("Read-only IVP SMMU candidate; identity does not establish a GPU route.");
        return info.valid == 7 ? 0 : 1;
    }
    if (argc == 2 && streq(argv[1], "gpu-dmss")) {
        MxDiagGpuDmss info{};
        const auto rc = sys_debug_read(MX_DIAG_EL3, 32, &info, sizeof(info));
        if (rc != sizeof(info) || info.version != MX_DIAG_VERSION) {
            error("GPU DMSS snapshot unavailable", rc);
            return 1;
        }
        hex("dmss.valid", info.valid);
        decimal("dmss.read_faults", info.faults);
        if (info.faults) {
            hex("dmss.first_esr", info.first_esr);
            hex("dmss.first_far", info.first_far);
        }
        if (info.valid & 1) {
            hex("dmss.rtl_version", info.global_version);
        }
        if (info.valid & 2) {
            hex("dmss.rtl_info", info.global_info);
        }
        if (info.valid & 4) {
            hex("dmss.gpu_master_config", info.pctrl_ctrl8);
        }
        bool complete = info.valid == 7;
        const char* names[] = {"region_count", "stat_mask", "stat_cycle", "filter0",
                               "filter1",      "flux_wr",   "flux_rd",    "sec_status"};
        for (uint32_t p = 0; p < MX_DIAG_DMSS_PORTS; ++p) {
            decimal("dmss.asi", p);
            hex("dmss.asi.valid", info.ports[p].valid);
            complete = complete && info.ports[p].valid == 255;
            for (uint32_t r = 0; r < MX_DIAG_DMSS_WORDS; ++r) {
                if (info.ports[p].valid & (1u << r)) {
                    hex(names[r], info.ports[p].words[r]);
                }
            }
        }
        putln("Read-only DMSS observations; counter activity alone does not identify a GPU route.");
        return complete ? 0 : 1;
    }
    if (argc == 2 && streq(argv[1], "gpu-iommu")) {
        return gpu_iommu_report();
    }
    if ((argc==3 || argc==4) && streq(argv[1],"gpu") && streq(argv[2],"cts")) {
        return cmd_diag_gpu_cts(argc==4 ? argv[3] : "dEQP-VK.info.*");
    }
    if(argc==3 && streq(argv[1],"gpu") && streq(argv[2],"cts-log"))return cmd_diag_gpu_cts(nullptr,true);
    if (argc == 3 && streq(argv[1], "gpu") && streq(argv[2], "vulkan")) {
        return cmd_diag_gpu_vulkan();
    }
    if ((argc == 2 || (argc == 3 && (streq(argv[2], "reset") || streq(argv[2], "upload")))) &&
        streq(argv[1], "gpu")) {
        return cmd_diag_gpu(argc == 3 && streq(argv[2], "reset"),
                            argc == 3 && streq(argv[2], "upload"));
    }
    if (argc >= 2 && streq(argv[1], "el3-rpmb")) {
        g.output_failed = false;
        const int result = el3_rpmb(argc, argv);
        return g.output_failed ? 1 : result;
    }
    if (argc >= 2 && streq(argv[1], "runtime")) {
        g.output_failed = false;
        return runtime_profile(argc, argv);
    }
    const bool all = argc < 2 || (argc == 2 && streq(argv[1], "all"));
    const bool el2 = argc == 2 && streq(argv[1], "el2");
    const bool el3 = argc == 2 && streq(argv[1], "el3");
    const bool el3_canary = argc == 2 && streq(argv[1], "el3-canary");
    const bool el3_recon_cmd = argc == 2 && streq(argv[1], "el3-recon");
    const bool el3_done_cmd = argc == 2 && streq(argv[1], "el3-done");
    const bool el3_bl31_cmd = argc == 2 && streq(argv[1], "el3-bl31read");
    const bool el3_gv_cmd = argc == 2 && streq(argv[1], "el3-gv");
    const bool is_special =
        (argc == 5 && streq(argv[1], "el3-aa")) || (argc == 3 && streq(argv[1], "el3-share")) ||
        (argc == 3 && streq(argv[1], "el3-ra")) || (argc == 2 && streq(argv[1], "el3-sharedump")) ||
        (argc == 5 && streq(argv[1], "el3-nn")) || (argc == 3 && streq(argv[1], "el3-cc")) ||
        (argc == 4 && streq(argv[1], "el3-cc2")) || (argc == 2 && streq(argv[1], "el3-aasweep")) ||
        (argc == 9 && streq(argv[1], "el3-socp")) || (argc == 4 && streq(argv[1], "el3-put")) ||
        (argc == 3 && streq(argv[1], "el3-poke")) ||
        ((argc >= 6 && argc <= 10) && streq(argv[1], "el3-svc")) ||
        (argc == 3 && streq(argv[1], "el3-win")) || (argc == 4 && streq(argv[1], "el3-wput")) ||
        (argc == 6 && streq(argv[1], "el3-scan")) ||
        ((argc >= 7 && argc <= 11) && streq(argv[1], "sac")) ||
        (argc == 2 && streq(argv[1], "sacstat")) || (argc == 3 && streq(argv[1], "el3-rawin")) ||
        (argc == 4 && streq(argv[1], "el3-rawout")) ||
        ((argc == 4 || argc == 5) && streq(argv[1], "el3-wmemset")) ||
        (argc == 3 && streq(argv[1], "el3-ipi")) || (argc == 3 && streq(argv[1], "el3-rprobe")) ||
        ((argc == 3 || argc == 4) && streq(argv[1], "el3-rmap")) ||
        (argc == 4 && streq(argv[1], "el3-rscan")) || (argc >= 3 && streq(argv[1], "el3tool")) ||
        (argc == 2 && streq(argv[1], "el3-teeos"));
    if (is_special) {
        g.output_failed = false;
        putln("=== diag begin ===");
        int r = 0;
        if (streq(argv[1], "el3-aa")) {
            r = el3_aa(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-share")) {
            r = el3_share(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-ra")) {
            r = el3_ra(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-sharedump")) {
            r = el3_sharedump();
        } else if (streq(argv[1], "el3-nn")) {
            r = el3_nn(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-cc")) {
            r = el3_cc(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-cc2")) {
            r = el3_cc2(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-aasweep")) {
            r = el3_aasweep();
        } else if (streq(argv[1], "el3-socp")) {
            r = el3_socp(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-put")) {
            r = el3_put(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-poke")) {
            r = el3_poke(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-svc")) {
            r = el3_svc(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-win")) {
            r = el3_win(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-wput")) {
            r = el3_wput(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-scan")) {
            r = el3_scan(argc - 1, argv + 1);
        } else if (streq(argv[1], "sac")) {
            r = sac_fire(argc - 1, argv + 1);
        } else if (streq(argv[1], "sacstat")) {
            r = sac_stat(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-rawin")) {
            r = el3_rawin(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-rawout")) {
            r = el3_rawout(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-wmemset")) {
            r = el3_wmemset(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-ipi")) {
            r = el3_ipi(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-rprobe")) {
            r = el3_rprobe(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-rmap")) {
            r = el3_rmap(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3-rscan")) {
            r = el3_rscan(argc - 1, argv + 1);
        } else if (streq(argv[1], "el3tool")) {
            r = el3tool(argc - 1, argv + 1);
        } else {
            r = el3_teeos();
        }
        line("=== diag end status=%u ===", static_cast<unsigned>(r));
        return g.output_failed ? 1 : r;
    }
    const bool log = argc == 2 && streq(argv[1], "log");
    if (argc > 2 || (!all && !el2 && !el3 && !el3_canary && !el3_recon_cmd && !el3_done_cmd &&
                     !el3_bl31_cmd && !el3_gv_cmd && !log)) {
        putln("usage: diag [all|el2|el3|el3-canary|el3-recon|el3-done|el3-bl31read|el3-gv|el3-aa "
              "<idx> <x1> <x2>|el3-sharedump|el3-share <off>|el3-ra <addr>|el3-nn|el3-teeos|log]");
        return 1;
    }
    g.output_failed = false;
    putln("=== diag begin ===");
    MxDiagSystem info{};
    int result = system_info(info) ? 0 : 1;
    if (!result) {
        if (all || el2) {
            result |= cpu_info(info);
        }
        if (all || el3) {
            result |= el3_info();
        }
        if (el3_canary) {
            result |= el3_info(true);
        }
        if (el3_recon_cmd) {
            result |= el3_recon();
        }
        if (el3_done_cmd) {
            result |= el3_done();
        }
        if (el3_bl31_cmd) {
            result |= el3_bl31read();
        }
        if (el3_gv_cmd) {
            result |= el3_gv();
        }
        if (all) {
            result |= runtime_info();
        }
        if (all || log) {
            result |= boot_log(info);
        }
    }
    if (g.output_failed) {
        result = 1;
    }
    line("=== diag end status=%u ===", static_cast<unsigned>(result));
    return g.output_failed ? 1 : result;
}

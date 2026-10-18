#include <cassert>
#include <cstring>
#include <string>
#include "mochi/syscall.h"
// The diagnostic parser also compiles the native sacrificial-call commands.
// Host report tests do not create threads or execute firmware calls.
inline int64_t sys_thread_get_cpu() { return 0; }
inline void sys_thread_exit(int32_t) {}
static unsigned host_thread_creates;
inline int64_t sys_thread_create(CapHandle, uint64_t, uint64_t, uint32_t) {
    ++host_thread_creates;
    return MX_ERR_NOT_IMPLEMENTED;
}
inline int64_t sys_thread_set_affinity(CapHandle, uint64_t) { return MX_ERR_NOT_IMPLEMENTED; }
inline int64_t sys_thread_resume(CapHandle) { return MX_ERR_NOT_IMPLEMENTED; }
inline mx_status_t mx_doorbell_create(CapHandle* out) { *out = 98; return MX_OK; }
static unsigned revoked;
#include "../userland/msh/msh_diag.cc"

Shell g{};
static std::string output, retained;
static uint64_t entry_el = 1, monitor = 0, capability = 1, capacity = 4096;
static unsigned reads, resources;
static bool fail_read, grow_log, bad_version, early_eof;
static bool el3_available = true;
static uint64_t canary_result;
static uint32_t done_aborted;
static uint64_t done_result;
static bool drop_output_once;
static bool bad_runtime_batch;
static MxDiagEl3Rpmb rpmb_model{};
static bool pump_mode, emit_next;
static unsigned pumped;
static int64_t read_diag(uint64_t request, uint64_t index, void* out, uint64_t size) {
    assert(size && size <= MX_DIAG_READ_MAX);
    ++reads;
    if (fail_read) return MX_ERR_NOT_IMPLEMENTED;
    if (request == MX_DIAG_RUNTIME) {
        MxDiagRuntimeBatch batch{};
        batch.version = MX_DIAG_VERSION;
        batch.first_pid = index;
        batch.next_pid = index + MX_DIAG_RUNTIME_ROWS <= MX_DIAG_PROCESS_LIMIT
            ? index + MX_DIAG_RUNTIME_ROWS : 0;
        batch.sampled_ns = host_now;
        batch.online_mask = 0xff;
        const bool after = host_now >= 1000000000;
        if (index == 1) {
            for (unsigned pid = 1; pid <= 7; ++pid) {
                if (pid == 6 && after) continue;
                auto& row = batch.rows[batch.count++];
                row.pid = pid;
                row.identity = ((pid == 3 && after ? 2ull : 1ull) << 32) | pid;
                row.runtime_ns = 100 + host_now / (pid == 2 ? 10 : 100);
                if (pid == 4 && after) row.runtime_ns = 0;
                if (pid == 5 && after) row.flags = 1;
                std::strcpy(row.name, pid == 2 ? "busy" : "quiet");
            }
        }
        if (bad_runtime_batch) batch.count = MX_DIAG_RUNTIME_ROWS + 1;
        std::memcpy(out, &batch, sizeof(batch));
        return sizeof(batch);
    }
    if (request == MX_DIAG_SYSTEM) {
        MxDiagSystem info{};
        info.version = bad_version ? 99 : MX_DIAG_VERSION;
        info.board = MX_DIAG_BOARD_ANNE;
        info.cpu_limit = 8;
        info.current_el = 1;
        info.pfr0 = capability << 8;
        info.online_mask = 0x55;
        info.boot_log_size = retained.size();
        info.boot_log_capacity = capacity;
        std::memcpy(out, &info, sizeof(info));
        return sizeof(info);
    }
    if (request == MX_DIAG_CPU) {
        assert(index < 8);
        MxDiagCpu cpu{};
        cpu.version = MX_DIAG_VERSION;
        cpu.cpu = index;
        cpu.online = (0x55 >> index) & 1;
        cpu.boot_entry_el = cpu.online ? entry_el : 0;
        cpu.monitor_ready = cpu.online ? monitor : 0;
        std::memcpy(out, &cpu, sizeof(cpu));
        return sizeof(cpu);
    }
    if (request == MX_DIAG_EL3) {
        if (index == 29) {
            if (!el3_available) return MX_ERR_NOT_IMPLEMENTED;
            const auto action = static_cast<MxDiagEl3Rpmb*>(out)->action;
            if (pump_mode && action == MX_RPMB_COMPLETE) {
                ++pumped;
                ++rpmb_model.completed_events;
                if (emit_next) { ++rpmb_model.events; emit_next = false; }
            }
            auto info = rpmb_model;
            info.version = bad_version ? 99 : MX_DIAG_VERSION;
            info.action = action;
            info.completion_error = 0xff05;
            std::memcpy(out, &info, sizeof(info));
            return sizeof(info);
        }
        if (index == 2) {
            MxDiagEl3Recon recon{};
            recon.version = MX_DIAG_VERSION;
            recon.status_6c42 = 0x5a42;
            std::memcpy(out, &recon, sizeof(recon));
            return sizeof(recon);
        }
        if (index == 3) {
            MxDiagEl3Done done{};
            done.version = MX_DIAG_VERSION;
            done.aborted = done_aborted;
            done.call_x0 = done_result;
            std::memcpy(out, &done, sizeof(done));
            return sizeof(done);
        }
        MxDiagEl3 info{};
        info.version = MX_DIAG_VERSION;
        info.available = el3_available;
        info.shared_base = 0x34a21000;
        info.request_address = 0x34a29000;
        info.request_address_result = canary_result;
        std::memcpy(out, &info, sizeof(info));
        return sizeof(info);
    }
    assert(request == MX_DIAG_BOOT_LOG);
    if (early_eof) return 0;
    assert(index < retained.size());
    const size_t count = std::min<size_t>(size, retained.size() - index);
    std::memcpy(out, retained.data() + index, count);
    if (grow_log) retained.append("new record\n");
    return count;
}
bool streq(const char* a, const char* b) { return libk::str::equal(a, b); }
void put_bytes(const char* p, uint32_t n) {
    if (drop_output_once) { drop_output_once = false; g.output_failed = true; return; }
    output.append(p, n);
}
void put(const char* p) { output.append(p); }
void putln(const char* p) { put(p); put("\r\n"); }
int cmd_bootinfo(int, char**) { ++resources; return 0; }
int cmd_cap(int, char**) { ++resources; return 0; }
int cmd_ring(int, char**) { ++resources; return 0; }
int cmd_doorbell(int, char**) { ++resources; return 0; }
int cmd_svc(int, char**) { ++resources; return 0; }

static int run(const char* argument = nullptr) {
    output.clear(); reads = resources = 0;
    char command[] = "diag";
    char* argv[] = {command, const_cast<char*>(argument)};
    return cmd_diag(argument ? 2 : 1, argv);
}
static bool contains(const char* p) { return output.find(p) != std::string::npos; }

static int run_rpmb(const char* action, const char* cpu = nullptr) {
    output.clear(); reads = resources = 0;
    char command[] = "diag", name[] = "el3-rpmb";
    char* argv[] = {command, name, const_cast<char*>(action), const_cast<char*>(cpu)};
    return cmd_diag(cpu ? 4 : action ? 3 : 2, argv);
}

int main() {
    host_on_debug_read = read_diag;
    host_on_cap_revoke = [](CapHandle) -> mx_status_t { ++revoked; return MX_OK; };
    // Real hardware may expose EL2 while firmware hands the kernel EL1.
    assert(run("el2") == 0 && contains("el2.hardware_field=1\r\n"));
    assert(contains("cpu0 online=1 mpidr=0x0 boot_entry_el=1 monitor_ready=0"));
    assert(contains("cpu7 online=0 mpidr=0x0 boot_entry_el=0 monitor_ready=0"));
    assert(resources == 0 && reads == 9);
    entry_el = 2; monitor = 1;
    assert(run("el2") == 0 && contains("boot_entry_el=2 monitor_ready=1"));
    assert(contains("host.current_el=1")); // Host drops to EL1 after monitor install.
    assert(run("el3") == 0 && contains("el3.shared_base=0x34a21000\r\n"));
    assert(contains("el3.rpmb_request=0x34a29000\r\n"));
    assert(contains("el3.rpmb_callback=undefined_in_hi6250_linux"));
    assert(!contains("el3.talk_callback_address="));
    assert(contains("EL3 probe queried metadata only"));
    assert(run("el3-recon") == 0 && contains("el3r.raw_6c42=0x5a42"));
    assert(!contains("el3r.status_6c42="));
    assert(run("el3-canary") == 0 && contains("registered the request address only"));
    assert(contains("EXT_CSD region bytes assumed"));
    canary_result = 0xff01;
    assert(run("el3-canary") == 1 && contains("EL3 canary rejected"));
    canary_result = ~uint64_t{0};
    assert(run("el3-canary") == 1);
    el3_available = false;
    assert(run("el3") == 1 && contains("unavailable on this board"));
    canary_result = 0;
    assert(run("el3-canary") == 1 && !contains("registered the request address"));
    el3_available = true;
    done_aborted = 1;
    assert(run("el3-done") == 1 && contains("aborted before the call"));
    done_aborted = 0; done_result = 0xff01;
    assert(run("el3-done") == 1);
    fail_read = true;
    output.clear();
    assert(el3_done() == 1 && contains("done-probe unavailable"));
    fail_read = false;

    // A CRLF straddles the read chunk boundary; LF alone is normalized.
    retained = std::string(MX_DIAG_READ_MAX - 1, 'x') + "\r\nend\n";
    const std::string expected = std::string(MX_DIAG_READ_MAX - 1, 'x') + "\r\nend\r\n";
    grow_log = true;
    assert(run("log") == 0 && output.find(expected) != std::string::npos);
    assert(!contains("\r\r\n") && !contains("new record") && reads == 3);
    grow_log = false;
    const auto original = retained;
    assert(run("log") == 0 && retained == original); // Non-consuming, repeatable.
    assert(run() == 0 && resources == 5 && contains("vm.guest_linux_hello=0"));
    assert(contains("memory.allocated_pages=0") && contains("=== diag end status=0 ==="));
    assert(run("nonsense") == 1 && reads == 0);
    drop_output_once = true;
    assert(run("el2") == 1 && contains("=== diag end status=1 ==="));
    assert(run("el2") == 0); // A fresh report clears an older transport failure.
    fail_read = true;
    assert(run() == 1 && contains("diagnostics unavailable") && resources == 0);
    fail_read = false; bad_version = true;
    assert(run() == 1 && reads == 1);
    bad_version = false; early_eof = true;
    assert(run("log") == 1 && contains("read incomplete"));
    early_eof = false; capacity = 0;
    assert(run("log") == 0 && contains("unavailable on this board") && reads == 1);
    capacity = retained.size();
    assert(run("log") == 0 && contains("capacity reached"));
    --capacity;
    assert(run("log") == 1 && contains("invalid retained log size") && reads == 1);
    host_now = 0;
    assert(run("runtime") == 0 && contains("runtime.wall_ns=5000000000"));
    assert(contains("runtime.excluded_processes=4"));
    assert(contains("name=busy cpu_ns=500000000 one_cpu_pct=10.00"));
    assert(output.find("pid=2 name=busy") < output.find("pid=1 name=quiet"));
    assert(contains("runtime.total_capacity_pct=1.50"));
    assert(resources == 0 && reads == 16); // No EL3, boot-log or cap traversal.
    bad_runtime_batch = true;
    assert(run("runtime") == 1 && contains("runtime snapshot unavailable"));

    assert(run_rpmb(nullptr) == 1 && reads == 0);
    assert(run_rpmb("status") == 0 && contains("rpmb.sgi5_events=0"));
    assert(run_rpmb("arm") == 0 && contains("EXT_CSD region bytes remain assumed"));
    assert(run_rpmb("arm-emmc") == 0);
    assert(run_rpmb("provision-key") == 0);
    assert(run_rpmb("key-status") == 0);
    rpmb_model.result = MX_RPMB_ALREADY_PROVISIONED;
    assert(run_rpmb("provision-key") == 1 && contains("already_provisioned"));
    rpmb_model.result = MX_RPMB_STORAGE_UNAVAILABLE;
    assert(run_rpmb("arm-emmc") == 1 && contains("storage_unavailable"));
    rpmb_model.result = MX_RPMB_REGISTRATION_FAILED;
    assert(run_rpmb("arm") == 1);
    rpmb_model.result = MX_RPMB_OK;
    rpmb_model.armed = rpmb_model.registered = 1;
    assert(run_rpmb("complete", "0") == 1 && reads == 0);
    assert(run_rpmb("counter", "1") == 1 && contains("CPU is unavailable"));
    assert(run_rpmb("complete", "2") == 1 && contains("exactly one fresh SGI 5"));
    assert(host_thread_creates == 0); // No thread/SMC without a real notification.
    rpmb_model.events = 1;
    assert(run_rpmb("complete", "2") == 1 && contains("RPMB worker create failed"));
    assert(host_thread_creates == 1);
    rpmb_model.done_started = rpmb_model.done_returned = 1;
    assert(run_rpmb("status") == 0 && contains("FF06 returned after a real notification"));
    assert(contains("rpmb.ff06_storage_result=0xff05"));
    assert(run_rpmb("complete", "2") == 1 && host_thread_creates == 1);
    rpmb_model = {};
    rpmb_model.armed = rpmb_model.registered = rpmb_model.storage_enabled = 1;
    assert(run_rpmb("serve", "2") == 1 && contains("worker create failed"));
    assert(host_thread_creates == 2);
    assert(run_rpmb("stop") == 0 && contains("not running"));
    pump_mode = emit_next = true;
    rpmb_model.events = 1;
    auto& worker = rpmb_workers[1];
    worker.cancel = 0;
    host_on_wait = []() -> int64_t { rpmb_workers[1].cancel = 1; return MX_OK; };
    assert(rpmb_serve(worker) == 0 && pumped == 2 && revoked == 1);
    assert(host_wait_deadline == host_now + 1000000 && worker.wake == kNullCap);
    worker.cancel = 0;
    rpmb_model.events += 2;
    assert(rpmb_serve(worker) == -static_cast<int64_t>(MX_RPMB_AMBIGUOUS_REQUEST) && pumped == 2 && revoked == 2);
    rpmb_model.events = rpmb_model.completed_events;
    host_on_wait = []() -> int64_t {
        if (!pumped) ++rpmb_model.events;
        else rpmb_workers[1].cancel = 1;
        return MX_OK;
    };
    pumped = 0;
    worker.cancel = 0;
    assert(rpmb_serve(worker) == 0 && pumped == 1 && revoked == 3);
    host_on_wait = nullptr;
    pump_mode = false;
    rpmb_model = {};
    rpmb_model.trigger_returned = 1;
    assert(run_rpmb("status") == 0 && contains("retail firmware may disable"));
    el3_available = false;
    assert(run_rpmb("status") == 1 && contains("lifecycle diagnostic unavailable"));
    el3_available = true;
    bad_version = true;
    assert(run_rpmb("status") == 1);
}

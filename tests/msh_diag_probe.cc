#include <cassert>
#include <cstring>
#include <string>
#include "../userland/msh/msh_diag.cc"

Shell g{};
static std::string output, retained;
static uint64_t entry_el = 1, monitor = 0, capability = 1, capacity = 4096;
static unsigned reads, resources;
static bool fail_read, grow_log, bad_version, early_eof;
static bool drop_output_once;
static int64_t read_diag(uint64_t request, uint64_t index, void* out, uint64_t size) {
    assert(size && size <= MX_DIAG_READ_MAX);
    ++reads;
    if (fail_read) return MX_ERR_NOT_IMPLEMENTED;
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

int main() {
    host_on_debug_read = read_diag;
    // Real hardware may expose EL2 while firmware hands the kernel EL1.
    assert(run("el2") == 0 && contains("el2.hardware_field=1\r\n"));
    assert(contains("cpu0 online=1 mpidr=0x0 boot_entry_el=1 monitor_ready=0"));
    assert(contains("cpu7 online=0 mpidr=0x0 boot_entry_el=0 monitor_ready=0"));
    assert(resources == 0 && reads == 9);
    entry_el = 2; monitor = 1;
    assert(run("el2") == 0 && contains("boot_entry_el=2 monitor_ready=1"));
    assert(contains("host.current_el=1")); // Host drops to EL1 after monitor install.

    // A CRLF straddles the 256-byte chunk boundary; LF alone is normalized.
    retained = std::string(255, 'x') + "\r\nend\n";
    const std::string expected = std::string(255, 'x') + "\r\nend\r\n";
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
}

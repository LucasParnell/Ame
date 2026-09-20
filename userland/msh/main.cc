// userland/msh/main.cc

#include "msh.h"

// msh entry point.  The interactive state and the line/token buffers live in
// the globals below (outside the 4 KiB initial thread stack); _start() maps
// BootInfo, attaches the terminal/FS/service endpoints, and runs the REPL.

Shell g{};

// Midix currently gives each initial userland thread a single 4 KiB stack.
// Keep the interactive line buffer and parsed token block out of the stack;
// ParsedLine alone is ~3 KiB.  Leaving it as a local in _start() underflowed
// the initial stack and faulted just below USER_STACK_TOP - PAGE_SIZE.
char g_line[kLineMax];
ParsedLine g_parsed{};

static bool map_ring(const MxBootInfo* boot, const char* name, mx_vaddr_t va,
                     RingPage** out_ring, uint32_t* out_order) {
    const MxBootEntry* e = mochi::boot::find_type(boot, name, MX_BOOT_ENTRY_RING);
    if (!e || e->handle == MX_NULL_HANDLE) return false;
    if (mx_map_page(e->handle, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) return false;
    if (out_ring) *out_ring = reinterpret_cast<RingPage*>(va);
    if (out_order) *out_order = e->aux0 ? static_cast<uint32_t>(e->aux0) : kDefaultOrder;
    return true;
}

static bool boot_map_term(const MxBootInfo* boot) {
    RingPage* input = nullptr;
    RingPage* output = nullptr;
    uint32_t input_order = kDefaultOrder;
    uint32_t output_order = kDefaultOrder;
    if (!map_ring(boot, "term.input.ring", kVaTermInput, &input, &input_order)) return false;
    if (!map_ring(boot, "term.output.ring", kVaTermOutput, &output, &output_order)) return false;
    if (input_order != output_order) {
        // libmochi::SpscEndpoint has one order field.  Init should create both
        // terminal rings with the same order.
        return false;
    }
    g.term.endpoint.rx_ring = input;
    g.term.endpoint.tx_ring = output;
    g.term.endpoint.order = input_order;
    g.term.endpoint.rx_doorbell = mochi::boot::handle(boot, "term.input.doorbell");
    g.term.endpoint.tx_doorbell = mochi::boot::handle(boot, "term.output.doorbell");
    g.have_terminal = g.term.valid();
    return g.have_terminal;
}

static bool boot_map_fs(const MxBootInfo* boot) {
    RingPage* cmd = nullptr;
    RingPage* reply = nullptr;
    uint32_t cmd_order = kDefaultOrder;
    uint32_t reply_order = kDefaultOrder;
    if (!map_ring(boot, "fs.cmd.ring", kVaFsCmdRing, &cmd, &cmd_order)) return false;
    if (!map_ring(boot, "fs.reply.ring", kVaFsReplyRing, &reply, &reply_order)) return false;
    if (cmd_order != reply_order) return false;
    g.fs.ep.tx_ring = cmd;
    g.fs.ep.rx_ring = reply;
    g.fs.ep.order = cmd_order;
    g.fs.ep.tx_doorbell = mochi::boot::handle(boot, "fs.cmd.doorbell");
    g.fs.ep.rx_doorbell = mochi::boot::handle(boot, "fs.reply.doorbell");
    g.have_fs = g.fs.valid();
    return g.have_fs;
}

static bool boot_map_svc_registry(const MxBootInfo* boot) {
    const mx_handle_t h = mochi::boot::handle(boot, "svc.registry");
    if (h == MX_NULL_HANDLE) return false;
    if (mx_map_page(h, kVaSvcRegistry, MX_RIGHT_READ) != MX_OK) return false;
    const auto* page = reinterpret_cast<const mochi::svc::RegistryPage*>(kVaSvcRegistry);
    if (!svc_registry_valid_snapshot(page)) return false;
    g.svc_registry = page;
    return true;
}

static bool boot_map_svcctl(const MxBootInfo* boot) {
    RingPage* cmd = nullptr;
    RingPage* reply = nullptr;
    uint32_t cmd_order = kDefaultOrder;
    uint32_t reply_order = kDefaultOrder;
    if (!map_ring(boot, "svc.cmd.ring", kVaFsReplyRing + 0x10000, &cmd, &cmd_order)) return false;
    if (!map_ring(boot, "svc.reply.ring", kVaFsReplyRing + 0x20000, &reply, &reply_order)) return false;
    if (cmd_order != reply_order) return false;
    g.svcctl.ep.tx_ring = cmd;
    g.svcctl.ep.rx_ring = reply;
    g.svcctl.ep.order = cmd_order;
    g.svcctl.ep.tx_doorbell = mochi::boot::handle(boot, "svc.cmd.doorbell");
    g.svcctl.ep.rx_doorbell = mochi::boot::handle(boot, "svc.reply.doorbell");
    g.have_svcctl = g.svcctl.valid();
    return g.have_svcctl;
}


static void prompt() {
    char buf[kPromptMax];
    build_prompt(buf, sizeof(buf));
    put(buf);
}

extern "C" [[noreturn]] void _start() {
    g.running = true;
    g.last_status = 0;
    g.cwd_handle = 1;
    copy_str(g.cwd, sizeof(g.cwd), "/");
    set_var("PWD", "/");
    set_var("SHELL", "msh");

    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    g.boot = boot;
    const bool log_ready = mochi::log_map_from_boot(boot, kVaLogRing, "msh");
    const bool term_ready = boot_map_term(boot);
    const bool fs_ready = boot_map_fs(boot);
    (void)boot_map_svc_registry(boot);
    (void)boot_map_svcctl(boot);
    mochi::boot::ready(boot);
    if (log_ready) {
        mochi::log_raw_marker(term_ready
            ? "msh: terminal attached\n"
            : "msh: terminal missing; using debug console fallback\n");
        if (!fs_ready) {
            mochi::log_raw_marker("msh: fs endpoint missing\n");
        }
    }

    putln("msh: Midix shell ready (libmochi BootInfo/rings/caps)");
    putln("type `help` for commands");

    while (g.running) {
        prompt();
        if (!read_line(g_line, sizeof(g_line))) break;
        if (!parse_line(g_line, &g_parsed)) {
            g.last_status = 2;
            continue;
        }
        g.last_status = execute_parsed(&g_parsed);
    }
    putln("logout");
    mx_exit(g.last_status);
    while (true) mx_yield();
}

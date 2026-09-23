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

// Map a manifest-granted endpoint: a tx/rx ring pair and their doorbells. Both
// rings must share an order (SpscEndpoint has a single order field).
static bool boot_map_endpoint(const MxBootInfo* boot,
                              const char* tx_name, mx_vaddr_t tx_va, const char* tx_db,
                              const char* rx_name, mx_vaddr_t rx_va, const char* rx_db,
                              mochi::SpscEndpoint* out) {
    uint32_t tx_order = kDefaultOrder;
    uint32_t rx_order = kDefaultOrder;
    RingPage* tx = mochi::boot::map_ring(boot, tx_name, tx_va, kDefaultOrder, &tx_order);
    RingPage* rx = mochi::boot::map_ring(boot, rx_name, rx_va, kDefaultOrder, &rx_order);
    const mx_handle_t tx_doorbell = mochi::boot::handle(boot, tx_db);
    const mx_handle_t rx_doorbell = mochi::boot::handle(boot, rx_db);
    if (!tx || !rx || tx_order != rx_order || tx_doorbell == MX_NULL_HANDLE ||
        rx_doorbell == MX_NULL_HANDLE) {
        return false;
    }
    *out = mochi::SpscEndpoint{tx, rx, tx_order, tx_doorbell, rx_doorbell};
    return out->valid();
}

static bool boot_map_term(const MxBootInfo* boot) {
    g.have_terminal = boot_map_endpoint(boot,
                                        "term.output.ring", kVaTermOutput, "term.output.doorbell",
                                        "term.input.ring", kVaTermInput, "term.input.doorbell",
                                        &g.term.endpoint);
    return g.have_terminal;
}

static bool boot_map_fs(const MxBootInfo* boot) {
    g.have_fs = boot_map_endpoint(boot,
                                  "fs.cmd.ring", kVaFsCmdRing, "fs.cmd.doorbell",
                                  "fs.reply.ring", kVaFsReplyRing, "fs.reply.doorbell",
                                  &g.fs.ep);
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
    g.have_svcctl = boot_map_endpoint(boot,
                                      "svc.cmd.ring", kVaSvcCmdRing, "svc.cmd.doorbell",
                                      "svc.reply.ring", kVaSvcReplyRing, "svc.reply.doorbell",
                                      &g.svcctl.ep);
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

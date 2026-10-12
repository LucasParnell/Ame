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

static bool boot_map_term(const MxBootInfo* boot) {
    g.term.endpoint = mochi::boot::map_endpoint(boot,
        {"term.output.ring", kVaTermOutput, "term.output.doorbell"},
        {"term.input.ring", kVaTermInput, "term.input.doorbell"});
    g.have_terminal = g.term.endpoint.valid();
    return g.have_terminal;
}

static bool boot_map_fs(const MxBootInfo* boot) {
    g.fs.ep = mochi::boot::map_endpoint(boot,
        {"fs.cmd.ring", kVaFsCmdRing, "fs.cmd.doorbell"},
        {"fs.reply.ring", kVaFsReplyRing, "fs.reply.doorbell"});
    g.have_fs = g.fs.ep.valid();
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
    g.svcctl.ep = mochi::boot::map_endpoint(boot,
        {"svc.cmd.ring", kVaSvcCmdRing, "svc.cmd.doorbell"},
        {"svc.reply.ring", kVaSvcReplyRing, "svc.reply.doorbell"});
    g.have_svcctl = g.svcctl.ep.valid();
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
    libk::str::copy_into(g.cwd, sizeof(g.cwd), "/");
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

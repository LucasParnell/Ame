// userland/msh/msh_builtins.cc

#include "msh.h"

// The builtin registry and the core builtins (help/exit/echo/printf/clear/set/unset).

const Builtin kBuiltins[] = {
    {"help", cmd_help, "show builtin commands"},
    {"container", cmd_container, "start/stop/status of the temporary USB app runner"},
    {"exit", cmd_exit, "exit msh"},
    {"echo", cmd_echo, "print arguments"},
    {"printf", cmd_printf, "print arguments without implicit newline unless requested"},
    {"clear", cmd_clear, "clear terminal by scrolling"},
    {"set", cmd_set, "set or list shell variables"},
    {"unset", cmd_unset, "unset a shell variable"},
    {"pwd", cmd_pwd, "print current shell directory"},
    {"cd", cmd_cd, "change shell directory"},
    {"ls", cmd_ls, "list directory entries"},
    {"cat", cmd_cat, "read an io.fs file"},
    {"mkdir", cmd_mkdir, "create an io.fs directory"},
    {"rm", cmd_rm, "remove an io.fs file or empty directory"},
    {"touch", cmd_touch, "create an empty io.fs file"},
    {"sync", cmd_sync, "flush durable filesystem writes"},
    {"fscheck", cmd_fscheck, "write/read a 3 MiB /data/storage-check.bin diagnostic"},
    {"bootinfo", cmd_bootinfo, "show BootInfo entries"},
    {"diag", cmd_diag, "diagnostics: all, el2, el3, log, runtime [milliseconds]"},
    {"svc", cmd_svc, "service registry: list/tree/groups/deps/log/start/stop/restart"},
    {"cap", cmd_cap, "capability tools: ls/tree/inspect/revoke"},
    {"arena", cmd_arena, "arena tools: ls/show"},
    {"ring", cmd_ring, "ring stats"},
    {"doorbell", cmd_doorbell, "doorbell stats"},
    {"vm", cmd_vm, "VM tools"},
    {"shutdown", cmd_shutdown, "power off the system"},
    {"reboot", cmd_reboot, "reset the system; fastboot/bootloader selects USB flashing"},
};


const uint32_t kBuiltinCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

int cmd_help(int, char**) {
    putln("msh builtins:");
    for (uint32_t i = 0; i < kBuiltinCount; ++i) {
        put("  ");
        put(kBuiltins[i].name);
        put(" - ");
        putln(kBuiltins[i].help);
    }
    putln("");
    putln("Midix commands:");
    putln("  svc list | svc tree | svc groups | svc deps NAME");
    putln("  svc log NAME | svc start NAME | svc stop NAME | svc restart NAME");
    putln("  cap ls | cap tree | cap inspect HANDLE | cap revoke HANDLE");
    putln("  arena ls | arena show NAME");
    putln("  ring stats | doorbell stats");
    putln("  diag [all|el2|el3|el3-canary|log]");
    putln("  diag runtime [100..30000 milliseconds]");
    putln("  shutdown | reboot [fastboot|bootloader]");
    putln("  vm list | vm start linux0 | vm console linux0");
    return 0;
}

int cmd_exit(int, char**) {
    g.running = false;
    return 0;
}

int cmd_echo(int argc, char** argv) {
    uint32_t start = 1;
    bool newline = true;
    if (argc > 1 && streq(argv[1], "-n")) {
        newline = false;
        start = 2;
    }
    for (uint32_t i = start; i < static_cast<uint32_t>(argc); ++i) {
        if (i != start) put(" ");
        put(argv[i]);
    }
    if (newline) put("\r\n");
    return 0;
}

int cmd_printf(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i != 1) put(" ");
        const char* s = argv[i];
        for (uint32_t j = 0; s[j]; ++j) {
            if (s[j] == '\\' && s[j + 1]) {
                ++j;
                if (s[j] == 'n') put("\r\n");
                else if (s[j] == 't') put("\t");
                else put_bytes(&s[j], 1);
            } else {
                put_bytes(&s[j], 1);
            }
        }
    }
    return 0;
}

int cmd_clear(int, char**) {
    for (uint32_t i = 0; i < 64; ++i) put("\r\n");
    return 0;
}

int cmd_set(int argc, char** argv) {
    if (argc == 1) {
        for (uint32_t i = 0; i < kVarMax; ++i) {
            if (!g.vars[i].used) continue;
            put(g.vars[i].name);
            put("=");
            putln(g.vars[i].value);
        }
        return 0;
    }
    for (int a = 1; a < argc; ++a) {
        if (!set_var_from_assignment(argv[a])) {
            put("msh: set: expected NAME=VALUE: ");
            putln(argv[a]);
            return 1;
        }
    }
    return 0;
}

int cmd_unset(int argc, char** argv) {
    if (argc < 2) return 0;
    for (int a = 1; a < argc; ++a) {
        EnvVar* v = find_var(argv[a]);
        if (v) libk::zero(*v);
    }
    return 0;
}


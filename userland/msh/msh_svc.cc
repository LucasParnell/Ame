// userland/msh/msh_svc.cc

#include "msh.h"

// Introspection builtins over BootInfo, the service registry, and the
// svcctl control ring: bootinfo/svc/cap/arena/ring/doorbell/vm/shutdown/reboot.

static void put_spaces(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) put(" ");
}

static void put_padded(const char* s, uint32_t width) {
    const uint32_t n = slen(s);
    put(s);
    if (n < width) put_spaces(width - n);
}

static void put_rule(uint32_t width) {
    for (uint32_t i = 0; i < width; ++i) put("-");
    put("\r\n");
}

// " label=" + a hex/decimal value: the shape every field below prints.
static void field_hex(const char* label, uint64_t v) { put(label); put_hex(v); }
static void field_dec(const char* label, uint64_t v) { put(label); put_dec(v); }

// Walk BootInfo entries, optionally filtered to one entry type. Every "list the
// things of type X" builtin is this loop with a different body.
template <typename Fn>
static void for_boot_entries(mx_u32_t type, Fn fn) {
    if (!g.boot) return;
    for (uint16_t i = 0; i < g.boot->entry_count; ++i) {
        const MxBootEntry& e = g.boot->entries[i];
        if (type != MX_BOOT_ENTRY_NONE && e.type != type) continue;
        fn(i, e);
    }
}

static const char* svc_group_name(const char* name) {
    if (!name) return "other";
    if (starts_with(name, "drv.")) return "drivers";
    if (starts_with(name, "demo.") || starts_with(name, "app.") || streq(name, "msh")) return "programs";
    return "services";
}


int cmd_bootinfo(int, char**) {
    if (!g.boot || !mochi::boot::valid(g.boot)) return 1;
    put("BootInfo entries: "); put_dec(g.boot->entry_count); put("\r\n");
    for_boot_entries(MX_BOOT_ENTRY_NONE, [](uint16_t i, const MxBootEntry& e) {
        put("  "); put_dec(i); put("  ");
        put(boot_type_name(e.type)); put("  ");
        put(e.name); field_hex("  h=", e.handle);
        if (e.aux0 || e.aux1) {
            field_hex(" aux0=", e.aux0);
            field_hex(" aux1=", e.aux1);
        }
        put("\r\n");
    });
    return 0;
}

static const char* svc_state_name(mx_u32_t state);

static void svc_print_section(const char* group_name) {
    put(group_name); put("\r\n");
    put_rule(73);
    put_padded("NAME", 24);
    put_padded("STATE", 10);
    put_padded("PRINCIPAL", 19);
    put_padded("RESTART", 10);
    putln("IMAGE");
    put_rule(73);
    if (!g.svc_registry) return;
    mochi::svc::RegistryEntry e{};
    const uint16_t count = svc_registry_entry_count();
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (!streq(svc_group_name(e.name), group_name)) continue;
        put_padded(e.name, 24);
        put_padded(svc_state_name(e.state), 10);
        put_padded(e.principal, 19);
        put_padded(e.restart, 10);
        putln(e.image);
    }
    put("\r\n");
}

static int svc_list() {
    if (!g.svc_registry) return notimpl("svc registry");
    svc_print_section("services");
    svc_print_section("drivers");
    svc_print_section("programs");
    return 0;
}

static int svc_groups() {
    putln("services  io.*, nameservice, tty.*, ui.*, dango.*");
    putln("drivers   drv.*");
    putln("programs  demo.*, app.*, msh");
    return 0;
}

static int svc_tree() {
    if (!g.svc_registry) return notimpl("svc registry");
    putln("userland");
    putln("├─ services");
    mochi::svc::RegistryEntry e{};
    const uint16_t count = svc_registry_entry_count();
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (streq(svc_group_name(e.name), "services")) { put("│  ├─ "); putln(e.name); }
    }
    putln("├─ drivers");
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (streq(svc_group_name(e.name), "drivers")) { put("│  ├─ "); putln(e.name); }
    }
    putln("└─ programs");
    for (uint16_t i = 0; i < count; ++i) {
        if (!svc_registry_copy_entry(i, &e)) continue;
        if (streq(svc_group_name(e.name), "programs")) { put("   ├─ "); putln(e.name); }
    }
    return 0;
}

static int svc_deps(const char* name) {
    if (!svc_exists(name)) { put("svc: not found: "); putln(name); return 1; }
    putln(name);
    if (streq(name, "msh")) {
        putln("├─ tty.fb0 terminal rings");
        putln("├─ io.fs command/reply rings");
        putln("└─ init svc.registry arena");
    } else if (streq(name, "tty.fb0")) {
        putln("├─ io.framebuffer");
        putln("├─ ui.input");
        putln("├─ io.console mirror");
        putln("└─ msh terminal rings");
    } else if (streq(name, "io.fs")) {
        putln("└─ drv.blk.virtio");
    } else if (streq(name, "dango.manager")) {
        putln("├─ vm.root");
        putln("└─ boot.initrd linux artifacts");
    } else {
        putln("└─ deps unavailable in static v0 registry");
    }
    return 0;
}

static const char* svc_state_name(mx_u32_t state) {
    if (state & mochi::svc::kStateRunning) return "running";
    if (state & mochi::svc::kStateSpawned) return "spawned";
    return "stopped";
}

static int svc_control(mx_u32_t op, const char* name, const char* verb) {
    if (!name) { put("usage: svc "); put(verb); putln(" NAME"); return 1; }
    if (!g.have_svcctl) return notimpl("svc control");
    mochi::svcctl::Reply reply{};
    if (!g.svcctl.request(op, name, &reply)) {
        putln("svc: control channel failure");
        return 1;
    }
    if (reply.status != MX_OK) {
        put("svc "); put(verb); put(" "); put(name); put(": "); put_status(reply.status); put("\r\n");
        return 1;
    }
    put("svc "); put(verb); put(" "); put(name); put(" -> "); putln(svc_state_name(reply.state));
    return 0;
}

static int system_control(mx_u32_t op, const char* verb) {
    if (!g.have_svcctl) return notimpl(verb);
    mochi::svcctl::Reply reply{};
    if (!g.svcctl.request(op, "system", &reply)) {
        put("msh: "); put(verb); putln(": control channel failure");
        return 1;
    }
    if (reply.status != MX_OK) {
        put("msh: "); put(verb); put(": "); put_status(reply.status); put("\r\n");
        return 1;
    }
    put(verb); putln(" requested");
    return 0;
}

int cmd_shutdown(int, char**) {
    return system_control(mochi::svcctl::kRequestShutdown, "shutdown");
}

int cmd_reboot(int, char**) {
    return system_control(mochi::svcctl::kRequestReboot, "reboot");
}

int cmd_svc(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "list")) return svc_list();
    if (streq(argv[1], "tree")) return svc_tree();
    if (streq(argv[1], "groups")) return svc_groups();
    if (streq(argv[1], "deps")) {
        if (argc < 3) { putln("usage: svc deps NAME"); return 1; }
        return svc_deps(argv[2]);
    }
    if (streq(argv[1], "log")) return notimpl("svc log");
    if (streq(argv[1], "start")) return svc_control(mochi::svcctl::kRequestStart, argc >= 3 ? argv[2] : nullptr, "start");
    if (streq(argv[1], "stop")) return svc_control(mochi::svcctl::kRequestStop, argc >= 3 ? argv[2] : nullptr, "stop");
    if (streq(argv[1], "restart")) return svc_control(mochi::svcctl::kRequestRestart, argc >= 3 ? argv[2] : nullptr, "restart");
    putln("usage: svc list|tree|groups|deps NAME|log NAME|start NAME|stop NAME|restart NAME");
    return 1;
}

static void print_cap_info(mx_handle_t h) {
    MxCapInfo info{};
    mx_status_t st = mx_cap_info(h, &info);
    if (st != MX_OK) {
        put_status(st);
        return;
    }
    put(cap_type_name(info.type));
    field_hex(" rights=", info.rights);
    field_hex(" base=", info.base);
    field_hex(" size=", info.size);
    if (info.attr) field_hex(" attr=", info.attr);
    if (info.flags) field_hex(" flags=", info.flags);
    if (info.aux0) field_hex(" aux0=", info.aux0);
}

int cmd_cap(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "ls")) {
        if (!g.boot) return 1;
        putln("BOOTINFO CAPABILITIES");
        for_boot_entries(MX_BOOT_ENTRY_NONE, [](uint16_t i, const MxBootEntry& e) {
            if (e.handle == MX_NULL_HANDLE) return;
            put("  "); put_dec(i); put(" "); put(e.name); put(" "); put_hex(e.handle); put(" ");
            print_cap_info(e.handle);
            put("\r\n");
        });
        return 0;
    }
    if (streq(argv[1], "tree")) {
        putln("msh cap tree: only BootInfo-visible caps are exposed in v0");
        return cmd_cap(1, argv);
    }
    if (streq(argv[1], "inspect")) {
        if (argc < 3) { putln("usage: cap inspect HANDLE"); return 1; }
        bool ok = false;
        mx_handle_t h = parse_u64(argv[2], &ok);
        if (!ok) { putln("cap inspect: bad handle"); return 1; }
        put_hex(h); put(" "); print_cap_info(h); put("\r\n");
        return 0;
    }
    if (streq(argv[1], "revoke")) {
        if (argc < 3) { putln("usage: cap revoke HANDLE"); return 1; }
        bool ok = false;
        mx_handle_t h = parse_u64(argv[2], &ok);
        if (!ok) { putln("cap revoke: bad handle"); return 1; }
        sys_cap_revoke(h);
        putln("revoked");
        return 0;
    }
    putln("usage: cap ls|tree|inspect HANDLE|revoke HANDLE");
    return 1;
}

int cmd_arena(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "ls")) {
        if (!g.boot) return 1;
        putln("ARENAS");
        for_boot_entries(MX_BOOT_ENTRY_ARENA, [](uint16_t, const MxBootEntry& e) {
            put("  "); put(e.name); field_hex(" ", e.handle);
            put(" "); print_cap_info(e.handle); put("\r\n");
        });
        return 0;
    }
    if (streq(argv[1], "show")) {
        if (argc < 3) { putln("usage: arena show NAME"); return 1; }
        if (g.svc_registry && svc_exists(argv[2])) {
            put(argv[2]); putln(": no per-service arena registry is exposed yet; boot arenas only:");
            char* fake_argv[2] = {argv[0], const_cast<char*>("ls")};
            return cmd_arena(2, fake_argv);
        }
        const MxBootEntry* e = mochi::boot::find_type(g.boot, argv[2], MX_BOOT_ENTRY_ARENA);
        if (!e) { put("arena not found: "); putln(argv[2]); return 1; }
        put(e->name); put(" "); put_hex(e->handle); put(" "); print_cap_info(e->handle); put("\r\n");
        return 0;
    }
    putln("usage: arena ls|show NAME");
    return 1;
}

static int cmd_ring_stats(int, char**) {
    if (!g.boot) return 1;
    putln("RINGS");
    for_boot_entries(MX_BOOT_ENTRY_RING, [](uint16_t, const MxBootEntry& e) {
        put("  "); put(e.name); field_dec(" order=", e.aux0 ? e.aux0 : kDefaultOrder);
        field_hex(" handle=", e.handle); put(" "); print_cap_info(e.handle); put("\r\n");
    });
    return 0;
}

int cmd_ring(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "stats")) return cmd_ring_stats(argc, argv);
    putln("usage: ring stats");
    return 1;
}

static int cmd_doorbell_stats(int, char**) {
    if (!g.boot) return 1;
    putln("DOORBELLS");
    for_boot_entries(MX_BOOT_ENTRY_DOORBELL, [](uint16_t, const MxBootEntry& e) {
        put("  "); put(e.name); field_hex(" handle=", e.handle);
        field_dec(" counter=", mx_doorbell_read(e.handle));
        put(" "); print_cap_info(e.handle); put("\r\n");
    });
    return 0;
}

int cmd_doorbell(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "stats")) return cmd_doorbell_stats(argc, argv);
    putln("usage: doorbell stats");
    return 1;
}

int cmd_vm(int argc, char** argv) {
    if (argc < 2 || streq(argv[1], "list")) {
        putln("NAME     STATE       DETAIL");
        put("linux0   ");
        const int64_t created = sys_debug_counter(dango::VM_DEBUG_CREATED);
        const int64_t channels = sys_debug_counter(dango::VM_DEBUG_CHANNELS_CREATED);
        if (created > 0) put("created     "); else put("uncreated   ");
        put("created="); put_dec(created > 0 ? static_cast<uint64_t>(created) : 0);
        put(" channels="); put_dec(channels > 0 ? static_cast<uint64_t>(channels) : 0);
        put("\r\n");
        return 0;
    }
    if (streq(argv[1], "start")) {
        putln("vm start: dango.manager in this tree starts linux0 during boot; no runtime VM manager command ring exists yet");
        return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
    }
    if (streq(argv[1], "console")) {
        putln("vm console: VM channel exists in kernel/dango, but no dango.manager console attach protocol exists yet");
        return static_cast<int>(MX_ERR_NOT_IMPLEMENTED);
    }
    putln("usage: vm list|start linux0|console linux0");
    return 1;
}


// Exercise the production shell exec path in a complete, headless system.
#include "../../userland/msh/msh.h"
#include "mochi/driver.h"
#include "mochi/wait.h"

Shell g{};
static void check(bool value, const char* why) {
    if (value)
        return;
    put("APP SYSTEM TEST FAILED: ");
    putln(why);
    mochi::park();
}
extern "C" [[noreturn]] void _start() {
    g.boot = mochi::boot::map();
    // Reproduce the terminal mappings held by a real msh session. A headless
    // harness without these reservations misses app-upload VA collisions.
    constexpr mx_u64_t terminal_marker = 0x53484c5445524d31;
    const auto terminal = sys_arena_create(4096, 0);
    check(terminal > 0 && mx_map_page(terminal, kVaTermInput, RIGHT_READ | RIGHT_WRITE) == MX_OK &&
              mx_map_page(terminal, kVaTermOutput, RIGHT_READ | RIGHT_WRITE) == MX_OK,
          "terminal address reservations");
    *reinterpret_cast<volatile mx_u64_t*>(kVaTermInput) = terminal_marker;
    libk::str::copy_into(g.cwd, sizeof(g.cwd), "/");
    g.fs.ep = mochi::boot::map_endpoint(g.boot, {"fs.cmd.ring", kVaFsCmdRing, "fs.cmd.doorbell"},
                                        {"fs.reply.ring", kVaFsReplyRing, "fs.reply.doorbell"});
    g.have_fs = g.fs.valid();
    check(g.have_fs, "filesystem mapping");
    check(cmd_exec("/data/apps/missing") != 0, "missing executable accepted");
    FsOpenReply data{}, bad{};
    check(g.fs.wait_open(1, "data", FS_OPEN_READ, &data) && data.status == MX_OK, "open data");
    check(g.fs.wait_open(data.handle, "bad.mxi", FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNC,
                         &bad) &&
              bad.status == MX_OK,
          "create bad image");
    FsWriteReply write{};
    check(g.fs.try_write(bad.handle, 0, "bad", 3, &write) && write.status == MX_OK,
          "write bad image");
    g.fs.try_close(bad.handle);
    g.fs.try_close(data.handle);
    check(cmd_exec("/data/bad.mxi") != 0, "bad image accepted");
#ifndef APP_PACKAGE_ONLY
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        putln("APP_SYSTEM_BENCH_BEGIN");
        check(cmd_exec("/ame/userland/bin/bench.ipc.mxi") == 0, "benchmark exit status");
        putln("APP_SYSTEM_BENCH_END");
        check(*reinterpret_cast<volatile mx_u64_t*>(kVaTermInput) == terminal_marker &&
                  *reinterpret_cast<volatile mx_u64_t*>(kVaTermOutput) == terminal_marker,
              "terminal mapping damaged by exec");
        check(g.fs.wait_open(1, "data", FS_OPEN_READ, &data) && data.status == MX_OK,
              "filesystem lost after exec");
        g.fs.try_close(data.handle);
    }
#endif
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        check(cmd_exec("/data/apps/package-demo.amp") == 0, "packaged app exit status");
        check(*reinterpret_cast<volatile mx_u64_t*>(kVaTermInput) == terminal_marker &&
                  *reinterpret_cast<volatile mx_u64_t*>(kVaTermOutput) == terminal_marker,
              "terminal mapping damaged by packaged exec");
        check(g.fs.wait_open(1, "data", FS_OPEN_READ, &data) && data.status == MX_OK,
              "filesystem lost after packaged exec");
        g.fs.try_close(data.handle);
    }
#ifdef APP_PACKAGE_ONLY
    for (unsigned cycle = 0; cycle < 2; ++cycle)
        check(cmd_exec("/data/apps/ui-demo.amp") == 0, "compiled QML app exit status");
    check(cmd_exec("/data/apps/bad-package.amp") != 0, "malformed package accepted");
    putln("app package system regression passed");
#else
    putln("app system regression passed");
#endif
    mochi::park();
}

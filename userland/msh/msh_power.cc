#include "msh.h"

// Init owns power authority. The shell sends a typed request and prints the
// returned status; it never guesses board registers or performs its own reset.
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

int cmd_shutdown(int argc, char**) {
    if (argc != 1) { putln("usage: shutdown"); return 1; }
    return system_control(mochi::svcctl::kRequestShutdown, "shutdown");
}

int cmd_reboot(int argc, char** argv) {
    if (argc == 1) return system_control(mochi::svcctl::kRequestReboot, "reboot");
    if (argc == 2 && (streq(argv[1], "fastboot") || streq(argv[1], "bootloader")))
        return system_control(mochi::svcctl::kRequestFastboot, "reboot fastboot");
    putln("usage: reboot [fastboot|bootloader]");
    return 1;
}

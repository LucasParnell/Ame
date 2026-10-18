#include "msh.h"
#include "mochi/driver.h"
#include "midix/abi/protocols/app_stream.h"

int cmd_container(int argc, char** argv) {
    using namespace midix::apps;
    mx_u32_t type = kStatus;
    if (argc != 2) { putln("usage: container start|stop|status"); return 1; }
    if (streq(argv[1], "start")) type = kStart;
    else if (streq(argv[1], "stop")) type = kStop;
    else if (!streq(argv[1], "status")) { putln("usage: container start|stop|status"); return 1; }
    if (!mochi::boot::handle(g.boot, "apps.control.tx")) {
        putln("container: USB app runner is unavailable in this system profile"); return 1;
    }
    if (!g.app_control.valid()) {
        g.app_control = mochi::driver::link(g.boot, "apps.control", 0x2c000000);
    }
    const auto& link = g.app_control;
    if (!link.valid()) { putln("container: control mapping failed"); return 1; }
    const auto now = mx_clock_ns();
    const Control request{now ? now : 1};
    const auto deadline = now + 5'000'000'000ull;
    if (!link.send_bounded(type, &request, sizeof(request), deadline)) {
        putln("container: runner did not accept the request"); return 1;
    }
    for (;;) {
        MxRingSlot slot{};
        if (!mochi::driver::receive(link, slot, deadline)) {
            putln("container: runner reply timed out"); return 1;
        }
        Reply reply{};
        if (!mochi::message_get(slot, &reply, kReply) || reply.token != request.token) continue;
        if (reply.status != MX_OK) { put("container: "); put_status(reply.status); putln(""); return 1; }
        if (type == kStop && reply.state == Stopping) {
            if (mx_clock_ns() >= deadline || !link.send_bounded(kStatus, &request, sizeof(request), deadline)) {
                putln("container: cleanup is still pending"); return 1;
            }
            mx_yield(); continue;
        }
        const char* states[] = {"closed", "ready: USB upload tunnel open", "receiving app",
                                "app running", "stopping: waiting for memory cleanup"};
        put("container: "); putln(reply.state <= Stopping ? states[reply.state] : "unknown state");
        if (reply.exit_code != kNoExit) {
            char line[64];
            libk::snprintf(line, sizeof(line), "last app exit: %d", reply.exit_code); putln(line);
        }
        return 0;
    }
}

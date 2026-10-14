#include "../userland/msh/msh_power.cc"
#include <cassert>
#include <string>

Shell g{};
static std::string output;
bool streq(const char* a, const char* b) { return libk::str::equal(a, b); }
void put(const char* p) { output.append(p); }
void putln(const char* p) { put(p); put("\r\n"); }
void put_status(mx_status_t status) { output += std::to_string(status); }
int notimpl(const char*) { return 1; }
struct Storage { MxRingPage page{}; MxRingSlot slots[8]{}; };
int main() {
    Storage request, reply;
    g.svcctl.ep = {&request.page, &reply.page, 3, 1, 2};
    g.have_svcctl = true;
    char reboot[] = "reboot", fastboot[] = "fastboot", bootloader[] = "bootloader", bad[] = "recovery";
    char* args[] = {reboot, fastboot, bad};
    auto invoke = [&](bool off, int argc, mx_status_t status, uint32_t expected) {
        const mochi::svcctl::Reply response{static_cast<mx_i32_t>(status), 0};
        assert(g.svcctl.ep.rx().try_push_msg(status == MX_OK ? mochi::svcctl::kReply : mochi::svcctl::kError, response));
        output.clear();
        const int result = off ? cmd_shutdown(argc, args) : cmd_reboot(argc, args);
        MxRingSlot slot{};
        assert(g.svcctl.ep.tx().try_pop(&slot) && slot.type == expected);
        mochi::svcctl::Request body{};
        assert(mochi::message_get(slot, &body) && streq(body.name, "system"));
        assert((result == 0) == (status == MX_OK));
    };
    invoke(false, 1, MX_OK, mochi::svcctl::kRequestReboot);
    invoke(false, 2, MX_OK, mochi::svcctl::kRequestFastboot);
    args[1] = bootloader;
    invoke(false, 2, MX_OK, mochi::svcctl::kRequestFastboot);
    invoke(true, 1, MX_OK, mochi::svcctl::kRequestShutdown);
    invoke(false, 1, MX_ERR_NOT_IMPLEMENTED, mochi::svcctl::kRequestReboot);
    assert(output.find("-10") != std::string::npos);
    args[1] = bad;
    assert(cmd_reboot(2, args) == 1 && cmd_reboot(3, args) == 1 && cmd_shutdown(2, args) == 1);
    MxRingSlot unused{}; assert(!g.svcctl.ep.tx().try_pop(&unused));
    g.have_svcctl = false;
    assert(cmd_reboot(1, args) == 1 && cmd_shutdown(1, args) == 1);
    static_assert(mochi::svcctl::power_operation(mochi::svcctl::kRequestShutdown) == MX_PLATFORM_SYSTEM_OFF);
    static_assert(mochi::svcctl::power_operation(mochi::svcctl::kRequestReboot) == MX_PLATFORM_SYSTEM_RESET);
    static_assert(mochi::svcctl::power_operation(mochi::svcctl::kRequestFastboot) == MX_PLATFORM_SYSTEM_FASTBOOT);
    static_assert(mochi::svcctl::power_operation(mochi::svcctl::kRequestStop) == 0);
}

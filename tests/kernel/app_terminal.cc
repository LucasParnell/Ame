// A serial peer for the production tty and msh, without a USB controller.
// Keep draining stdout while the benchmark runs, then prove new input works.
#include "mochi/boot.h"
#include "mochi/driver.h"
#include "mochi/terminal_session.h"
#include "mochi/wait.h"
#include "libk/str.h"

namespace {
mochi::terminal::SessionSink input;
mochi::Ring output;
char tail[192]{};
mx_u32_t used;
char capture_line[2048]{};
mx_u32_t capture_used;
mx_u64_t generation = 1;
mx_handle_t idle_wake;
bool done, exited;

void flush_capture() {
    if (capture_used) {
        sys_debug_puts(capture_line, capture_used);
        capture_used = 0;
    }
}

[[noreturn]] void fail(const char* phase) {
    constexpr char prefix[] = "APP TERMINAL TEST FAILED: ";
    sys_debug_puts(prefix, sizeof(prefix) - 1);
    sys_debug_puts(phase, libk::str::length(phase));
    sys_debug_puts("\n", 1);
    mochi::park();
}
bool ends(const char* text) {
    const auto size = libk::str::length(text);
    return size <= used && libk::str::equal(tail + used - size, text);
}
void receive(const char* text, bool benchmark = false) {
    const auto deadline = mx_clock_ns() + 15'000'000'000ull;
    for (;;) {
        MxRingSlot slot{};
        if (!output.try_pop(&slot)) {
            if (mx_clock_ns() >= deadline)
                fail(text);
            // Exercise a sleeping peer, including the wake following app exit.
            mochi::SpscEndpoint reader{};
            reader.rx_ring = output.ring;
            reader.order = output.order;
            mochi::driver::idle(&reader, 1, output.doorbell, mx_clock_ns() + 1'000'000);
            continue;
        }
        mx_u64_t session = 0;
        libk::const_bytes bytes;
        if (!mochi::terminal::session_bytes(slot, MX_TERM_SESSION_OUTPUT_BYTES, &session, &bytes) ||
            session != generation)
            continue;
        for (mx_u32_t i = 0; i < bytes.size; ++i) {
            // Emit complete lines atomically to the debug UART. Logging each
            // serial packet allowed unrelated boot diagnostics inside JSON.
            capture_line[capture_used++] = bytes.data[i];
            if (bytes.data[i] == '\n' || capture_used == sizeof(capture_line))
                flush_capture();
            if (used == sizeof(tail) - 1) {
                for (mx_u32_t j = 1; j < used; ++j)
                    tail[j - 1] = tail[j];
                --used;
            }
            tail[used++] = bytes.data[i];
            tail[used] = 0;
            done |= ends("IPC_BENCH_DONE");
            exited |= ends("msh: exec: child exited, status=0");
            if (ends(text) && (!benchmark || (done && exited))) {
                flush_capture();
                return;
            }
        }
    }
}
void control(mx_u32_t code) {
    const auto message =
        mochi::terminal::session_control(code, code == MX_TERM_CONTROL_FLOW_ON ? generation : 0);
    if (!input.push_msg(MX_TERM_CONTROL, message))
        fail("session control");
}
void send(const char* command) {
    // Let msh finish writing its prompt and park before publishing new input.
    // Otherwise a busy peer could keep every terminal read on its try path.
    const auto settled = mx_clock_ns() + 20'000'000;
    while (mx_clock_ns() < settled)
        (void)mx_doorbell_wait_until(idle_wake, 0, settled);
    used = 0;
    tail[0] = 0;
    const auto size = libk::str::length(command);
    mx_u32_t offset = 0;
    const auto deadline = mx_clock_ns() + 1'000'000'000;
    while (offset < size) {
        offset += input.try_write(MX_TERM_SESSION_INPUT_BYTES, generation,
                                  libk::as_bytes(command + offset, size - offset));
        if (mx_clock_ns() >= deadline)
            fail("send command");
        mx_yield();
    }
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    input = {mochi::boot::map_ring(boot, {"serial.in.ring", 0x20000000, "serial.in.doorbell"})};
    output = mochi::boot::map_ring(boot, {"serial.out.ring", 0x20020000, "serial.out.doorbell"});
    if (!input.valid() || !output.valid())
        fail("serial mapping");
    const auto wake = sys_doorbell_create();
    if (wake <= 0)
        fail("idle doorbell");
    idle_wake = wake;
    if (mx_doorbell_notify(mochi::boot::require_handle(boot, "console.activate")) != MX_OK)
        fail("activate terminal");
    control(MX_TERM_CONTROL_FLOW_ON);
    mochi::boot::ready(boot);
    receive("msh:/$ ");
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        done = exited = false;
        send("/ame/userland/bin/bench.ipc.mxi\r");
        receive("msh:/$ ", true);
        send("echo APP_TERMINAL_INPUT_WORKS\r");
        // The echoed command alone cannot satisfy this: require the output
        // line followed by the next prompt.
        receive("\r\nAPP_TERMINAL_INPUT_WORKS\r\nmsh:/$ ");
        if (!cycle) {
            control(MX_TERM_CONTROL_FLOW_OFF);
            ++generation;
            control(MX_TERM_CONTROL_FLOW_ON);
            receive("msh:/$ ");
            send("\x04");
            receive("logout\r\n");
            receive("msh:/$ ");
        }
    }
    constexpr char passed[] = "\napp terminal regression passed\n";
    sys_debug_puts(passed, sizeof(passed) - 1);
    mochi::park();
}

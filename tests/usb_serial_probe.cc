// Production raw-serial bridge, terminal transport and renderer. Exercise
// ownership under full rings and connection changes, with no phone involved.
#include "../userland/usb/serial/bridge.h"
#include "../userland/drivers/tty/transport.h"
#include "../userland/drivers/tty/output.h"
#include <cassert>
#include <cstdio>
#include <cstring>
struct Storage {
    MxRingPage page{};
    MxRingSlot slots[8]{};
};
struct Fixture {
    Storage from_usb, to_usb, to_tty, from_tty, to_shell;
    mochi::usb::serial::Runtime bridge{};
    mochi::tty::InputBuffer input{};
    mochi::tty::SerialMirror mirror{};
    mochi::terminal::InputSink shell;
    Fixture() : shell{{&to_shell.page, 3, 1}} {
        bridge.stream = {&to_usb.page, &from_usb.page, 3, 1, 1};
        bridge.input = {{&to_tty.page, 3, 1}};
        bridge.output = {&from_tty.page, 3, 1};
        mirror.output = {{&from_tty.page, 3, 1}};
    }
    void change(mx_u64_t generation, bool configured) {
        const MxFunctionChunk reset{
            generation, 0, 0, 0, static_cast<mx_u8_t>(configured ? MX_FUNCTION_CONFIGURED : 0), {}};
        assert(bridge.stream.rx().try_push_msg(MX_FUNCTION_RESET, reset));
        assert(mochi::usb::serial::accept_host(bridge));
        assert(bridge.lifecycle_pending);
        assert(mochi::usb::serial::accept_host(bridge));
        MxRingSlot slot{};
        assert(bridge.input.try_pop(&slot));
        assert(mochi::tty::accept_serial(slot, mirror, input));
        assert(mirror.generation == (configured ? generation : 0));
        assert(input.retained() && input.flush(shell));
        assert(shell.try_pop(&slot) && slot.type == MX_TERM_CONTROL);
        MxTermControl control{};
        assert(mochi::message_get(slot, &control, MX_TERM_CONTROL));
        assert(control.code == (configured ? MX_TERM_CONTROL_FLOW_ON : MX_TERM_CONTROL_FLOW_OFF));
    }
};
static void input_backpressure() {
    Fixture f;
    f.change(11, true);
    MxFunctionChunk chunk{11, 1, 4, 0, 0, {'h', 'e', 'l', 'p'}};
    const char filler = 'x';
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.bridge.input.try_push(MX_TERM_INPUT_BYTES, &filler, 1));
    }
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RX, chunk));
    assert(mochi::usb::serial::accept_host(f.bridge) && f.bridge.rx_held);
    assert(!mochi::usb::serial::accept_host(f.bridge)); // blocked is not progress
    assert(!(mochi::usb::serial::readable_mask(f.bridge) & 1));
    MxRingSlot slot{};
    assert(f.bridge.input.try_pop(&slot));
    assert(mochi::usb::serial::accept_host(f.bridge) && !f.bridge.rx_held);
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.bridge.input.try_pop(&slot));
    }
    assert(mochi::tty::accept_serial(slot, f.mirror, f.input));
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.shell.try_write_bytes(&filler, 1));
    }
    assert(!f.input.flush(f.shell) && f.input.pending.remaining() == 4);
    assert(!f.input.hold(libk::as_bytes(&filler, 1))); // suffix still owns the buffer
    assert(f.shell.try_pop(&slot) && f.input.flush(f.shell));
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.shell.try_pop(&slot));
    }
    assert(slot.length == 4 && !std::memcmp(slot.payload, "help", 4));
    assert(f.input.pending.empty());
}
static void output_generations() {
    Fixture f;
    f.change(21, true);
    // The host has configured its terminal before initiating output.
    MxFunctionChunk input{21, 1, 1, 0, 0, {'\r'}};
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RX, input));
    assert(mochi::usb::serial::accept_host(f.bridge));
    MxRingSlot admitted{};
    assert(f.bridge.input.try_pop(&admitted));
    MxRingSlot raw{};
    raw.type = MX_TERM_OUTPUT_BYTES;
    raw.length = 48;
    for (unsigned i = 0; i < 48; ++i) {
        raw.payload[i] = 'a' + i % 26;
    }
    assert(f.mirror.hold(raw) && f.mirror.flush());
    assert(f.bridge.output.depth() == 2); // 40 + 8 terminal session bytes
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    mx_u8_t received[48];
    unsigned count = 0;
    MxRingSlot slot{};
    MxFunctionChunk chunk{};
    while (f.bridge.stream.tx().try_pop(&slot)) {
        assert(mochi::message_get(slot, &chunk, MX_FUNCTION_TX));
        assert(chunk.generation == 21 && count + chunk.length <= sizeof(received));
        std::memcpy(received + count, chunk.data, chunk.length);
        count += chunk.length;
    }
    assert(count == 48 && !std::memcmp(received, raw.payload, count));
    // Old bytes can be queued on either side of a reset; neither queue may
    // relabel them with the new generation, even if the controller stays up.
    assert(f.mirror.hold(raw) && f.mirror.flush());
    f.change(22, true);
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(!f.bridge.stream.tx().depth() && f.bridge.dropped_output == 48);
    assert(f.mirror.hold(raw) && f.mirror.flush());
    f.change(23, false);
    assert(!f.bridge.configured && !f.mirror.generation);
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(!f.bridge.stream.tx().depth());
    assert(f.mirror.hold(raw) && f.mirror.pending.empty());
}
static void no_unsolicited_output_before_host_input() {
    Fixture f;
    f.change(25, true);
    MxRingSlot raw{};
    raw.type = MX_TERM_OUTPUT_BYTES;
    raw.length = 7;
    std::memcpy(raw.payload, "msh:/$ ", 7);
    assert(f.mirror.hold(raw) && f.mirror.flush());
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(!f.bridge.stream.tx().depth() && f.bridge.dropped_output == 7);
    MxFunctionChunk input{25, 1, 1, 0, 0, {'\r'}};
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RX, input));
    assert(mochi::usb::serial::accept_host(f.bridge));
    MxRingSlot admitted{};
    assert(f.bridge.input.try_pop(&admitted));
    assert(f.mirror.hold(raw) && f.mirror.flush());
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(f.bridge.stream.tx().depth());
    f.change(26, true);
    assert(!f.bridge.host_started);
}
static void lifecycle_backpressure() {
    Fixture f;
    f.change(31, true);
    const char filler = 'x';
    for (unsigned i = 0; i < 8; ++i) {
        assert(f.shell.try_write_bytes(&filler, 1));
    }
    MxRingSlot slot{};
    const auto off = mochi::terminal::session_control(MX_TERM_CONTROL_FLOW_OFF, 0);
    assert(mochi::message_put(&slot, MX_TERM_CONTROL, off));
    assert(mochi::tty::accept_serial(slot, f.mirror, f.input));
    assert(!f.mirror.generation && f.input.retained());
    assert(!f.input.flush(f.shell));
    assert(!f.input.hold(libk::as_bytes(&filler, 1)));
    const auto on = mochi::terminal::session_control(MX_TERM_CONTROL_FLOW_ON, 32);
    assert(mochi::message_put(&slot, MX_TERM_CONTROL, on));
    assert(!mochi::tty::accept_serial(slot, f.mirror, f.input));
    assert(!f.mirror.generation); // retain the old control until delivery
    MxRingSlot drained{};
    assert(f.shell.try_pop(&drained) && f.input.flush(f.shell));
    assert(mochi::tty::accept_serial(slot, f.mirror, f.input));
    assert(f.mirror.generation == 32 && !f.input.flush(f.shell));
    for (unsigned i = 0; i < 7; ++i) {
        assert(f.shell.try_pop(&drained));
    }
    assert(f.shell.try_pop(&drained));
    MxTermControl control{};
    assert(mochi::message_get(drained, &control, MX_TERM_CONTROL));
    assert(control.code == MX_TERM_CONTROL_FLOW_OFF);
    assert(f.input.flush(f.shell) && !f.input.retained());
    assert(f.shell.try_pop(&drained));
    assert(mochi::message_get(drained, &control, MX_TERM_CONTROL));
    assert(control.code == MX_TERM_CONTROL_FLOW_ON);
}
static void render_backpressure_and_scroll() {
    Storage commands;
    char cells[15];
    mochi::tty::Render render{};
    render.fb.cmd = {&commands.page, 3, 1};
    render.cells = cells;
    render.rows = 3;
    render.cols = 5;
    render.scale = 1;
    render.cell_w = 8;
    render.cell_h = 16;
    render.init();
    MxRingSlot slot{};
    while (render.pending()) {
        assert(render.flush());
        while (render.fb.cmd.try_pop(&slot)) {
        }
    }
    render.write(libk::as_bytes("aa\nbb\ncc\n", 9));
    assert(render.first_row == 1);
    assert(!std::memcmp(render.line(0), "bb   ", 5));
    assert(!std::memcmp(render.line(1), "cc   ", 5));
    assert(!std::memcmp(render.line(2), "     ", 5));
    for (unsigned i = 0; i < 8; ++i) {
        assert(render.fb.cmd.try_push(FB_PRESENT, nullptr, 0));
    }
    assert(!render.flush() && render.dirty_count == 3);
    while (render.fb.cmd.try_pop(&slot)) {
    }
    unsigned draws = 0;
    while (render.pending()) {
        assert(render.flush(1)); // each dirty range advances only after admission
        while (render.fb.cmd.try_pop(&slot)) {
            if (slot.type == FB_DRAW_TEXT) {
                FbDrawTextCmd draw{};
                assert(mochi::message_get(slot, &draw, FB_DRAW_TEXT));
                assert(draw.len > 0 && draw.len <= 5);
                const auto row = draw.y / 16;
                const auto col = draw.x / 8;
                assert(row < 3 && !std::memcmp(draw.text, render.line(row) + col, draw.len));
                ++draws;
            }
        }
    }
    assert(draws == 3);
    render.write(libk::as_bytes("xy", 2));
    assert(render.dirty_count == 1); // contiguous printable bytes coalesce
    libk::ByteCursor cursor;
    cursor.reset(libk::as_bytes("abc", 3));
    assert(!cursor.advance(4) && cursor.remaining() == 3);
    assert(cursor.advance(2) && cursor.next(32).size == 1);
}
static void stalled_usb_cannot_hold_the_local_prompt() {
    Fixture f;
    f.change(61, true);
    const char filler = 'z';
    for (unsigned i = 0; i < 8; ++i)
        assert(f.mirror.output.try_write(MX_TERM_SESSION_OUTPUT_BYTES, 61,
                                        libk::as_bytes(&filler, 1)) == 1);
    Storage shell_output;
    mochi::terminal::OutputSink producer{{&shell_output.page, 3, 1}};
    mochi::terminal::OutputSource source{{&shell_output.page, 3, 1}};
    char log[384], cells[1280];
    std::memset(log, 'x', sizeof(log));
    std::memcpy(log + sizeof(log) - 9, "\r\nmsh:/$ ", 9);
    assert(producer.try_write_bytes(log, sizeof(log)));
    assert(!producer.try_write_bytes(&filler, 1)); // shell producer is backpressured too
    mochi::tty::Render render{};
    render.cells = cells;
    render.rows = 32; render.cols = 40;
    render.scale = 1; render.cell_w = 8; render.cell_h = 16;
    render.init();
    assert(mochi::tty::service_shell_output(source, f.mirror, render, true, 1));
    assert(source.depth() == 7 && !f.mirror.pending.empty());
    assert(!mochi::tty::service_shell_output(source, f.mirror, render, true, 1000000000));
    assert(source.depth() == 7); // preserve a bounded wait for an ordinary slow host
    assert(mochi::tty::service_shell_output(source, f.mirror, render, true, 1000000001));
    assert(!source.depth() && f.mirror.pending.empty());
    assert(render.x == 7 && !std::memcmp(render.line(render.y), "msh:/$ ", 7));
    assert(producer.try_write_bytes(&filler, 1)); // the next local command can write
    assert(f.mirror.output_stalled && f.mirror.dropped_output == sizeof(log));
    MxRingSlot slot{};
    while (f.mirror.output.try_pop(&slot)) {}
    f.change(62, true);
    assert(!f.mirror.output_stalled);
    assert(mochi::tty::service_shell_output(source, f.mirror, render, true, 1000000002));
    assert(f.mirror.output.try_pop(&slot));
    mx_u64_t generation = 0;
    libk::const_bytes bytes;
    assert(mochi::terminal::session_bytes(slot, MX_TERM_SESSION_OUTPUT_BYTES, &generation, &bytes));
    assert(generation == 62 && bytes.size == 1 && bytes.data[0] == filler);
}
static void slow_serial_progress_preserves_the_pending_suffix() {
    Fixture f;
    f.change(71, true);
    const char filler = 'z';
    for (unsigned i = 0; i < 8; ++i)
        assert(f.mirror.output.try_write(MX_TERM_SESSION_OUTPUT_BYTES, 71,
                                        libk::as_bytes(&filler, 1)) == 1);
    MxRingSlot raw{}, slot{};
    raw.type = MX_TERM_OUTPUT_BYTES;
    raw.length = 48;
    std::memset(raw.payload, 'a', raw.length);
    assert(f.mirror.hold(raw));
    assert(!f.mirror.flush(10));
    assert(f.mirror.output.try_pop(&slot));
    assert(f.mirror.flush(999999999) && f.mirror.pending.remaining() == 8);
    // The accepted 40-byte prefix restarted the budget for its owned suffix.
    assert(!f.mirror.flush(1000000010) && !f.mirror.output_stalled);
    assert(f.mirror.output.try_pop(&slot));
    assert(f.mirror.flush(1000000011) && f.mirror.pending.empty());
    assert(!f.mirror.output_stalled && !f.mirror.dropped_output);
    unsigned actual = 0;
    while (f.mirror.output.try_pop(&slot)) {
        mx_u64_t generation = 0;
        libk::const_bytes bytes;
        assert(mochi::terminal::session_bytes(slot, MX_TERM_SESSION_OUTPUT_BYTES, &generation, &bytes));
        if (bytes.data[0] == 'a') actual += bytes.size;
    }
    assert(actual == raw.length);
}
// A serial host must be able to wake its own console. The tty gate used to need
// three ESC keycodes from a keyboard, which a serial-only board cannot send, so
// the FLOW_ON lifecycle record was dropped by the same predicate that closed the
// gate and the console stayed deaf forever. Pin the property at the transport
// layer: control is accepted unconditionally and its generation is what opens
// the gate, with no keyboard involvement anywhere in the path.
void serial_attach_opens_console() {
    Fixture f;
    const mx_u64_t generation = 7;
    f.change(generation, true);
    // The mirror now carries a live generation, which is exactly the condition
    // tty.fb0's console_live() reads as "a host is attached".
    assert(f.mirror.generation == generation);
    // Bytes for this generation must then be deliverable without any keypress.
    MxFunctionChunk chunk{generation, 1, 5, 1, 0, {}};
    std::memcpy(chunk.data, "hello", 5);
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RX, chunk));
    assert(mochi::usb::serial::accept_host(f.bridge));
    // The bridge republishes host bytes as a session-input record stamped with
    // the generation the mirror holds; that match is what authorises delivery.
    MxRingSlot slot{};
    assert(f.bridge.input.try_pop(&slot));
    mx_u64_t delivered = 0;
    libk::const_bytes bytes;
    assert(mochi::terminal::session_bytes(slot, MX_TERM_SESSION_INPUT_BYTES, &delivered, &bytes));
    assert(delivered == generation && bytes.size == 5);
    assert(mochi::tty::accept_serial(slot, f.mirror, f.input));
    assert(f.input.retained());
}

// A host that re-opens the port it already owns sends a reset that keeps the
// same generation and stays configured. Treating that as "host has not started"
// silences output until an inbound byte arrives, and deliver_shell discards -
// not queues - everything in the meantime. That is a permanent byte loss, and
// it is what made the CDC console go mute while bulk input still worked.
static void host_reopen_keeps_output_alive() {
    Fixture f;
    f.change(41, true);
    MxFunctionChunk input{41, 1, 1, 0, 0, {'\r'}};
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RX, input));
    assert(mochi::usb::serial::accept_host(f.bridge));
    MxRingSlot admitted{};
    assert(f.bridge.input.try_pop(&admitted));
    assert(f.bridge.host_started);

    MxRingSlot raw{};
    raw.type = MX_TERM_OUTPUT_BYTES;
    raw.length = 6;
    std::memcpy(raw.payload, "before", 6);
    assert(f.mirror.hold(raw) && f.mirror.flush());
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(f.bridge.dropped_output == 0);

    // Same generation, still configured: a reconnect, not a new session.
    const MxFunctionChunk reopen{41, 0, 0, 0, MX_FUNCTION_CONFIGURED, {}};
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RESET, reopen));
    assert(mochi::usb::serial::accept_host(f.bridge));
    assert(f.bridge.host_started);

    raw.length = 5;
    std::memcpy(raw.payload, "after", 5);
    assert(f.mirror.hold(raw) && f.mirror.flush());
    assert(mochi::usb::serial::deliver_shell(f.bridge));
    assert(f.bridge.dropped_output == 0);
    assert(f.bridge.stream.tx().depth());
}
// A RESET must be readable even while a forwarded row is retained. Otherwise a
// blocked downstream ring latches rx_held forever with no way out: the reset is
// what clears the latch, and a retained row used to block the only read that
// could observe it.
static void reset_reaches_held_row() {
    Fixture f;
    f.change(51, true);
    char filler = 'x';
    while (f.bridge.input.try_push(MX_TERM_INPUT_BYTES, &filler, 1)) {
    }
    MxFunctionChunk chunk{51, 1, 4, 0, 0, {'h', 'e', 'l', 'p'}};
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RX, chunk));
    assert(mochi::usb::serial::accept_host(f.bridge) && f.bridge.rx_held);
    // Blocked with no reset pending: still no progress, nothing consumed.
    assert(!mochi::usb::serial::accept_host(f.bridge));
    assert(f.bridge.rx_held && f.bridge.generation == 51);
    // A reset for a new generation must break the latch.
    const MxFunctionChunk reset{52, 0, 0, 0, MX_FUNCTION_CONFIGURED, {}};
    assert(f.bridge.stream.rx().try_push_msg(MX_FUNCTION_RESET, reset));
    assert(mochi::usb::serial::accept_host(f.bridge));
    assert(f.bridge.generation == 52 && !f.bridge.rx_held);
}
int main() {
    stalled_usb_cannot_hold_the_local_prompt();
    slow_serial_progress_preserves_the_pending_suffix();
    no_unsolicited_output_before_host_input();
    host_reopen_keeps_output_alive();
    reset_reaches_held_row();
    serial_attach_opens_console();
    input_backpressure();
    output_generations();
    lifecycle_backpressure();
    render_backpressure_and_scroll();
    std::puts("usb serial and terminal probe ok");
}

// Production raw-serial bridge, terminal transport and renderer. Exercise
// ownership under full rings and connection changes, with no phone involved.
#include "../userland/usb/serial/bridge.h"
#include "../userland/drivers/tty/transport.h"
#include "../userland/drivers/tty/render.h"
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
                assert(draw.len == 5);
                const auto row = draw.y / 16;
                assert(row < 3 && !std::memcmp(draw.text, render.line(row), draw.len));
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
int main() {
    input_backpressure();
    output_generations();
    render_backpressure_and_scroll();
    std::puts("usb serial and terminal probe ok");
}

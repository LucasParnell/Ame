#define _start unused_router_start
#include "../userland/debug/usb/main.cc"
#undef _start
#include <cassert>
#include <cstdio>
#include <cstring>
struct Storage { MxRingPage page{}; MxRingSlot slots[8]{}; };
int main() {
    Storage host_rx, host_tx, apps_rx, apps_tx, keys;
    Runtime r{};
    r.link = {&host_tx.page, &host_rx.page, 3, 1, 1};
    r.apps = {&apps_tx.page, &apps_rx.page, 3, 1, 1};
    r.input = {{&keys.page, 3, 1}};
    MxFunctionChunk reset{7, 0, 0, 0, MX_FUNCTION_CONFIGURED, {}};
    assert(r.link.rx().try_push_msg(MX_FUNCTION_RESET, reset));
    for (int i = 0; i < 4; ++i) service(r);
    MxRingSlot slot{};
    assert(r.apps.tx().try_pop(&slot) && slot.type == MX_FUNCTION_RESET);
    mx_u8_t frame[80]{};
    libk::write_le<mx_u32_t>(libk::as_bytes(frame, 4), midix::debug::kInputMagic);
    libk::write_le<mx_u32_t>(libk::as_bytes(frame + 4, 4), 1);
    libk::write_le<mx_u16_t>(libk::as_bytes(frame + 8, 2), midix::apps::kData);
    libk::write_le<mx_u16_t>(libk::as_bytes(frame + 10, 2), 64);
    for (unsigned i = 16; i < 80; ++i) frame[i] = i;
    libk::write_le<mx_u32_t>(libk::as_bytes(frame + 12, 4),
                           midix::debug::checksum(libk::as_bytes(frame)));
    // Occupy the forwarding ring: a complete frame must remain owned by the
    // router until the runner accepts every byte, without becoming a key frame.
    for (int i = 0; i < 8; ++i) assert(r.apps.tx().try_push_msg(MX_FUNCTION_RX, reset));
    for (unsigned i = 0; i < 80; i += 32) {
        const unsigned n = 80 - i < 32 ? 80 - i : 32;
        MxFunctionChunk chunk{7, 0, static_cast<mx_u16_t>(n), 0, 0, {}};
        std::memcpy(chunk.data, frame + i, n);
        assert(r.link.rx().try_push_msg(MX_FUNCTION_RX, chunk));
    }
    for (int i = 0; i < 16; ++i) service(r);
    assert(r.source.app_pending && keys.page.head.v == 0);
    for (int i = 0; i < 8; ++i) assert(r.apps.tx().try_pop(&slot));
    for (int i = 0; i < 16; ++i) service(r);
    mx_u8_t actual[80]{};
    unsigned offset = 0;
    while (r.apps.tx().try_pop(&slot)) {
        MxFunctionChunk chunk{};
        assert(mochi::message_get(slot, &chunk, MX_FUNCTION_RX));
        assert(offset + chunk.length <= 80);
        std::memcpy(actual + offset, chunk.data, chunk.length);
        offset += chunk.length;
    }
    assert(offset == 80 && std::memcmp(frame, actual, 80) == 0);
    assert(libk::is_zero(libk::as_bytes(r.source.parser.bytes)));
    // Old connection replies are discarded; current ones reach the USB peer.
    MxFunctionChunk reply{6, 1, 1, 0, 0, {'x'}};
    assert(r.apps.rx().try_push_msg(MX_FUNCTION_TX, reply));
    service(r); assert(!r.link.tx().try_pop(&slot));
    reply.generation = 7;
    assert(r.apps.rx().try_push_msg(MX_FUNCTION_TX, reply));
    service(r); assert(r.link.tx().try_pop(&slot));
    reset.generation = 8; reset.flags = 0;
    assert(r.link.rx().try_push_msg(MX_FUNCTION_RESET, reset));
    for (int i = 0; i < 4; ++i) service(r);
    assert(r.apps.tx().try_pop(&slot) && slot.type == MX_FUNCTION_RESET);
    std::puts("app USB router probe ok");
}

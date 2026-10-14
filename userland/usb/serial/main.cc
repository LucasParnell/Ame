// userland/usb/serial/main.cc — dev.serial: raw serial console for the shell.
//
// Presents the CDC ACM function as a plain serial line: host bytes become
// shell terminal input and the shell's output byte stream returns to the host.
// No framing — any serial terminal (screen, picocom, PuTTY) drives the shell
// directly. Keyboard-code framing stays on the bulk function (dev.usb-input);
// this service owns only the ACM stream and moves opaque bytes.
//
// Backpressure: one received chunk is retained until the shell input ring
// accepts it (the host sees USB backpressure through the gadget), and one
// shell-output slot is retained while its 32-byte chunks drain to the
// function. Output produced with no host connection is dropped, never
// replayed stale across reconnects.
#include "mochi/driver.h"
#include "mochi/state.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "mochi/ring.h"
#include "mochi/terminal.h"
#include "midix/abi/protocols/usb_function.h"
namespace {
// MxFunctionChunk carries at most 32 inline bytes; the gadget enforces the same.
constexpr mx_u32_t kChunkMax = 32;
constexpr mx_u32_t kChunksPerTurn = 32;

struct Runtime {
    mochi::SpscEndpoint stream;                 // usb.fn.acm stream (debug_acm_a/b)
    mochi::terminal::InputSink shell_input;     // bytes -> shell (tty.fb0 -> msh)
    mochi::terminal::OutputSource shell_output; // shell bytes -> host
    MxFunctionChunk rx;                         // retained host->shell chunk
    bool rx_held;
    RingSlot tx;                                // retained shell->host slot
    mx_u32_t tx_offset;
    bool tx_held;
    mx_u64_t generation;                        // 0 until the gadget reports one
    mx_u32_t tx_sequence;
    mx_u64_t dropped_output;                    // no connection: discarded bytes
};

// Host -> shell. Consume one function chunk at a time; retain it when the
// shell input ring is full so the gadget keeps the host's OUT transfer
// pending instead of losing bytes silently.
bool accept_host(Runtime& r) {
    if (!r.rx_held) {
        MxRingSlot slot{};
        if (!r.stream.try_recv(&slot)) return false;
        if (slot.type == MX_FUNCTION_RESET) {
            MxFunctionChunk reset{};
            if (mochi::message_get(slot, &reset, slot.type)) {
                r.generation = reset.generation;
                r.tx_held = false; // the host connection was replaced
            }
            return true;
        }
        if (!mochi::message_get(slot, &r.rx, MX_FUNCTION_RX) ||
            r.rx.length > kChunkMax || !r.rx.length) {
            return true;
        }
        if (r.generation && r.rx.generation != r.generation) {
            r.tx_held = false; // reconnect without a reset: drop stale output
        }
        r.generation = r.rx.generation;
        r.rx_held = true;
    }
    if (r.shell_input.try_write_bytes(r.rx.data, r.rx.length)) {
        r.rx_held = false;
    }
    return true;
}

// Shell -> host. Retain one output slot and emit 32-byte chunks while the
// function link has room. Chunks carry the current connection generation:
// the gadget refuses anything else, so stale output cannot cross a reconnect.
bool deliver_shell(Runtime& r) {
    bool progress = false;
    if (!r.tx_held) {
        RingSlot slot{};
        if (r.shell_output.try_read(&slot)) {
            if (!r.generation) {
                r.dropped_output += slot.length; // no host connection yet
                return true;
            }
            r.tx = slot;
            r.tx_offset = 0;
            r.tx_held = true;
            progress = true;
        }
    }
    mx_u32_t budget = kChunksPerTurn;
    while (r.tx_held && r.generation && budget--) {
        const auto left = r.tx.length - r.tx_offset;
        MxFunctionChunk chunk{};
        chunk.generation = r.generation;
        chunk.sequence = ++r.tx_sequence;
        chunk.length = static_cast<mx_u16_t>(left < kChunkMax ? left : kChunkMax);
        libk::copy_n(libk::as_bytes(chunk.data),
                     libk::slice(libk::as_bytes(r.tx.payload, r.tx.length), r.tx_offset,
                                 chunk.length),
                     chunk.length);
        if (!r.stream.tx().try_push_msg(MX_FUNCTION_TX, chunk)) break;
        r.tx_offset += chunk.length;
        if (r.tx_offset == r.tx.length) r.tx_held = false;
        progress = true;
    }
    return progress;
}

bool service(Runtime& r) {
    const bool inbound = accept_host(r);
    const bool outbound = deliver_shell(r);
    return inbound || outbound;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    const auto* boot = mochi::boot::map();
    (void)mochi::log_map_from_boot(boot, mochi::va::native_driver::kLog, "dev.serial");
    auto arena = mochi::make_state(mochi::va::native_driver::kState, 16384);
    auto* r = arena.alloc<Runtime>();
    if (!r) {
        mochi::boot::invalid_boot();
    }
    r->stream = mochi::driver::link(boot, "stream", mochi::va::native_driver::kLinks);
    r->shell_input = {mochi::boot::map_ring(
        boot, {"serial.in.ring",
               (mochi::va::native_driver::kLinks + 2 * mochi::va::native_driver::kLinkStride),
               "serial.in.doorbell"})};
    r->shell_output = {mochi::boot::map_ring(
        boot, {"serial.out.ring",
               (mochi::va::native_driver::kLinks + 3 * mochi::va::native_driver::kLinkStride),
               "serial.out.doorbell"})};
    if (!r->stream.valid() || !r->shell_input.valid() || !r->shell_output.valid()) {
        mochi::boot::invalid_boot();
    }
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch([&] { return service(*r); })) {
            mx_yield();
            continue;
        }
        // Both producers notify one console doorbell: usb.fn.acm when host
        // bytes arrive and tty.fb0 when the shell writes output. Exclude a
        // retained direction so queued data cannot turn idle into a busy loop.
        mochi::SpscEndpoint readers[2]{};
        readers[0].rx_ring = r->stream.rx_ring;
        readers[0].order = r->stream.order;
        readers[1].rx_ring = r->shell_output.ring;
        readers[1].order = r->shell_output.order;
        const bool blocked = r->rx_held || r->tx_held;
        const mx_u64_t readable = (!r->rx_held ? 1u : 0u) | (!r->tx_held ? 2u : 0u);
        mochi::driver::idle(readers, 2, r->stream.rx_doorbell,
                            blocked ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs : 0,
                            readable);
    }
}

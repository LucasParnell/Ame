#pragma once
#include "hid.h"
#include "mochi/driver.h"
#include "mochi/fido2_prompt.h"
#include "mochi/log.h"
#include "midix/abi/protocols/usb_function.h"
namespace mochi::fido2 {
struct Runtime {
    mochi::SpscEndpoint link;
    Hid hid;
    Presence presence;
    libk::Mmio gpio;
    PromptPublisher prompt;
    mx_u64_t generation = 0;
    bool configured = false;
    mx_u8_t incoming[64]{}, outgoing[64]{};
    mx_u32_t incoming_bytes = 0, outgoing_chunk = 2;

    // Arena::alloc returns storage without running C++ initializers. Establish
    // the idle sentinel before any queued host request can be serviced.
    void initialize() {
        outgoing_chunk = 2;
        hid.reset();
    }
    void presence_changed(bool was_waiting) {
        if (was_waiting == hid.waiting) return;
        if (hid.waiting) {
            presence.reset(mx_clock_ns());
            prompt.start(generation, mx_clock_ns() | 1,
                         hid.auth.selection_only ? MX_FIDO2_SELECT : hid.auth.command, hid.auth.rp_id);
            mochi::log_raw_marker("waiting for physical volume-up approval\n");
        } else {
            const auto status = hid.auth.response[0];
            const bool selected = hid.auth.selection_only && (status == 0x31 || status == 0x35);
            prompt.finish(selected ? MX_FIDO2_SELECTED
                          : status == 0 || status == 0x19 ? MX_FIDO2_APPROVED
                          : status == 0x2d ? MX_FIDO2_CANCELLED
                          : status == 0x2f ? MX_FIDO2_EXPIRED : MX_FIDO2_REJECTED);
            mochi::log_counter("operation.status", status);
        }
    }

    bool service() {
        bool progress = prompt.flush();
        // The controller receives an entire HID report, never a partial stream
        // packet. Retain the second IPC chunk until the gadget has capacity.
        if (outgoing_chunk < 2) {
            MxFunctionChunk chunk{generation, outgoing_chunk, 32, 0x86, 0, {}};
            libk::copy_n(libk::as_bytes(chunk.data),
                         libk::as_bytes(outgoing + outgoing_chunk * 32, 32), 32);
            if (link.tx().try_push_msg(MX_FUNCTION_TX, chunk)) { ++outgoing_chunk; progress = true; }
        }
        if (outgoing_chunk == 2 && hid.report(libk::as_bytes(outgoing))) {
            outgoing_chunk = 0;
            progress = true;
        }
        // Never consume an additional report while its response is retained.
        // RESET can queue behind old traffic; generation validation in the
        // gadget fences every obsolete response until that notice is consumed.
        if (outgoing_chunk == 2 && !hid.output_pending) {
            MxRingSlot slot{};
            if (link.try_recv(&slot)) {
                progress = true;
                MxFunctionChunk chunk{};
                if (mochi::message_get(slot, &chunk, MX_FUNCTION_RESET) &&
                    chunk.generation >= generation && !(chunk.flags & ~MX_FUNCTION_CONFIGURED)) {
                    generation = chunk.generation;
                    configured = chunk.flags & MX_FUNCTION_CONFIGURED;
                    hid.reset(); incoming_bytes = 0;
                } else if (mochi::message_get(slot, &chunk, MX_FUNCTION_RX) && configured &&
                           chunk.generation == generation && chunk.endpoint == 6 &&
                           !chunk.flags && chunk.length == 32) {
                    if (chunk.sequence == 0) incoming_bytes = 0;
                    if (chunk.sequence == incoming_bytes / 32 && incoming_bytes < 64) {
                        libk::copy_n(libk::as_bytes(incoming + incoming_bytes, 32), libk::as_bytes(chunk.data), 32);
                        incoming_bytes += 32;
                        if (incoming_bytes == 64) {
                            const bool was_waiting = hid.waiting;
                            const auto transactions = hid.transactions;
                            hid.receive(libk::as_bytes(incoming), mx_clock_ns());
                            presence_changed(was_waiting);
                            if (transactions != hid.transactions && hid.auth.command != 4) {
                                mochi::log_counter("ctap.command", hid.auth.command);
                                if (hid.auth.command == 6)
                                    mochi::log_counter("pin.command", member(hid.auth.request,2).number);
                                if (!hid.waiting) mochi::log_counter("ctap.status", hid.auth.response[0]);
                            }
                            incoming_bytes = 0;
                        }
                    } else incoming_bytes = 0;
                }
            }
        }
        if (hid.waiting || hid.assembling) {
            bool press = false;
            if (hid.waiting) {
                mx_u32_t direction = 0, data = 0;
                // PL061 masked-data register for bit 1. Verify input direction
                // on every sample and leave pinmux/clocks under board ownership.
                if (gpio.read32(0x400, &direction) && !(direction & 2) && gpio.read32(8, &data))
                    press = presence.sample(!(data & 2), mx_clock_ns());
                else presence.reset(mx_clock_ns());
            }
            const bool was_waiting = hid.waiting;
            hid.tick(mx_clock_ns(), press);
            presence_changed(was_waiting);
        }
        return progress;
    }
    mx_u64_t deadline() const {
        if (prompt.pending || outgoing_chunk < 2 || hid.output_pending) return mx_clock_ns() + mochi::driver::kBackpressureRetryNs;
        if (hid.waiting) return mx_clock_ns() + 5'000'000; // only poll the button during approval
        return hid.assembling ? hid.deadline : 0;
    }
};
} // namespace mochi::fido2

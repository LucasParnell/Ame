#pragma once
// Bounded byte-function routing over private, non-overlapping payload slices.
// A received OUT slice cannot be reused until every inline chunk is published;
// a function IN chunk stays held until the controller reports completion.
#include "ep0.h"
namespace mochi::usb::gadget {
inline bool service_functions(Runtime& r) {
    bool progress = false;
    MxRingSlot slot{};
    if (r.demand_pending) {
        const MxFido2Demand demand{r.function_generation, r.fido2_requested ? 1u : 0u, 0};
        if (r.fido2_demand.try_push_msg(MX_FIDO2_DEMAND, demand)) {
            r.demand_pending = false;
            progress = true;
        }
    }
    for (mx_u32_t i = 0; i < 3; ++i) {
        auto& f = r.functions[i];
        const bool hid = i == 2;
        const mx_u8_t out_ep = hid ? 6 : i ? 4 : 1, in_ep = hid ? 0x86 : i ? 0x85 : 0x82;
        if (f.reset && (!hid || r.fido2_requested)) {
            const MxFunctionChunk reset{
                r.function_generation,
                0,
                0,
                0,
                static_cast<mx_u8_t>(r.configuration ? MX_FUNCTION_CONFIGURED : 0),
                {}};
            if (r.links[i + 1].tx().try_push_msg(MX_FUNCTION_RESET, reset)) {
                f.reset = false;
                progress = true;
            } else {
                // The peer may hold old output while this reset notice waits
                // for room. Drain obsolete replies/bytes to break that cycle.
                // Fresh-generation traffic cannot precede the lifecycle notice.
                for (mx_u32_t n = 0; n < driver::kBatchLimit && r.links[i + 1].try_recv(&slot);
                     ++n) {
                    progress = true;
                }
                continue;
            }
        }
        while (f.rx_pending) {
            MxFunctionChunk chunk{r.function_generation, hid ? f.forwarded / 32 : 0, 0,
                                  out_ep, 0, {}};
            const auto left = f.received - f.forwarded;
            chunk.length = left < MX_FUNCTION_INLINE_BYTES ? left : MX_FUNCTION_INLINE_BYTES;
            libk::copy_n(libk::as_bytes(chunk.data),
                         libk::slice(r.payload, 1024 + i * 1024 + f.forwarded, chunk.length),
                         chunk.length);
            if (r.links[i + 1].tx().try_push_msg(MX_FUNCTION_RX, chunk)) {
                progress = true;
                f.forwarded += chunk.length;
                if (f.forwarded == f.received) {
                    f.rx_pending = false;
                }
            } else {
                break; // retain the unforwarded payload until the function drains space
            }
        }
        if (r.configuration && !f.rx_pending && !f.rx_active) {
            if (packet(r, MX_DEVICE_RECEIVE, out_ep, 1024 + i * 1024,
                       !hid && r.speed == MxUsbSpeed::High ? 512 : 64, 0, &f.rx_id)) {
                f.rx_active = true;
                progress = true;
            }
        }
        // Fill one endpoint packet from currently ready inline chunks. Do not
        // delay a partial packet waiting for more bytes; do not touch its slice
        // while the controller owns it. This removes per-32-byte USB transfers.
        const mx_u32_t packet_bytes = !hid && r.speed == MxUsbSpeed::High ? 512 : 64;
        for (mx_u32_t n = 0; n < driver::kBatchLimit && !f.tx_active &&
                             f.tx_bytes + MX_FUNCTION_INLINE_BYTES <= packet_bytes &&
                             r.links[i + 1].try_recv(&slot);
             ++n) {
            progress = true;
            {
                MxFunctionChunk chunk{};
                if (mochi::message_get(slot, &chunk, MX_FUNCTION_TX) &&
                    chunk.length <= MX_FUNCTION_INLINE_BYTES && !chunk.flags &&
                    chunk.generation == r.function_generation && r.configuration && chunk.length &&
                    (!hid || (r.fido2_requested && chunk.length == 32 &&
                              chunk.sequence == f.tx_bytes / 32))) {
                    libk::copy_n(libk::slice(r.payload, 1536 + i * 1024 + f.tx_bytes, chunk.length),
                                 libk::as_bytes(chunk.data), chunk.length);
                    f.tx_bytes += chunk.length;
                    f.tx_pending = true;
                }
            }
        }
        if (f.tx_pending && !f.tx_active && r.configuration && (!hid || f.tx_bytes == 64) &&
            packet(r, MX_DEVICE_SEND, in_ep, 1536 + i * 1024, f.tx_bytes, 0, &f.tx_id)) {
            f.tx_active = true;
            progress = true;
        }
    }
    return progress;
}
} // namespace mochi::usb::gadget

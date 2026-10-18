#pragma once
// Gadget event-loop orchestration. EP0, function transport and composition
// data have separate modules; no MMIO, serial/keyboard or board policy here.
#include "functions.h"
namespace mochi::usb::gadget {
inline bool service(Runtime& r) {
    bool progress = false;
    MxRingSlot slot{};
    for (mx_u32_t n = 0; n < driver::kBatchLimit && r.links[0].try_recv(&slot); ++n) {
        progress = true;
        MxDeviceEvent event{};
        if (!mochi::message_get(slot, &event, slot.type)) {
            continue;
        }
        if (slot.type == MX_DEVICE_SETUP) {
            setup(r, event);
        } else if (slot.type == MX_DEVICE_COMPLETE) {
            completion(r, event);
        } else if (slot.type == MX_DEVICE_RESET || slot.type == MX_DEVICE_DISCONNECT) {
            r.generation = event.generation;
            r.configuration = 0;
            r.phase = ControlPhase::Idle;
            r.ep0_active = false;
            r.pending_function = false;
            r.address_pending = false;
            r.configure_next = kFunctionEndpointCount;
            reset_functions(r);
        }
    }
    if (r.pending_function && r.links[1].tx().try_push_msg(MX_FUNCTION_SETUP, r.function_request)) {
        r.pending_function = false;
        progress = true;
    }
    if (r.configuration && r.configure_next < kFunctionEndpointCount) {
        progress |= configure(r);
        // Preserve CONFIGURE -> status SEND ordering under ring pressure.
        if (r.configure_next < kFunctionEndpointCount) {
            return progress;
        }
    }
    if (!r.ep0_active) {
        if (r.phase == ControlPhase::In) {
            const auto remaining = r.length - r.sent, length = remaining < 64 ? remaining : 64;
            if (packet(r, MX_DEVICE_SEND, 128, 512 + r.sent, length)) {
                r.ep0_active = true;
                progress = true;
            }
        } else if (r.phase == ControlPhase::StatusIn) {
            // The controller stages the address for this setup identity and
            // programs it with the status IN packet. Scheduling it after the
            // ACK leaves a process-hop delay after the host may use the new
            // address. Keep ADDRESS -> SEND ordered on this SPSC transport.
            if (r.address_pending) {
                if (!packet(r, MX_DEVICE_ADDRESS, 0, 0, 0, r.pending_address)) {
                    return progress;
                }
                r.address_pending = false;
                progress = true;
            }
            if (packet(r, MX_DEVICE_SEND, 128, 512, 0)) {
                r.ep0_active = true;
                progress = true;
            }
        } else if (r.phase == ControlPhase::StatusOut || r.phase == ControlPhase::OutClass) {
            if (packet(r, MX_DEVICE_RECEIVE, 0, 0,
                       r.phase == ControlPhase::OutClass ? r.setup.setup.length : 0)) {
                r.ep0_active = true;
                progress = true;
            }
        } else if (r.phase == ControlPhase::Stall && packet(r, MX_DEVICE_STALL, 0, 0, 0)) {
            r.phase = ControlPhase::Idle;
            progress = true;
        }
    }
    return service_functions(r) || progress;
}

inline bool retry_pending(const Runtime& r) {
    if (r.demand_pending || r.pending_function || (r.configuration && r.configure_next < kFunctionEndpointCount)) {
        return true;
    }
    if (!r.ep0_active && r.phase != ControlPhase::Idle && r.phase != ControlPhase::Function) {
        return true;
    }
    for (mx_u32_t i = 0; i < 3; ++i) {
        const auto& function = r.functions[i];
        if ((function.reset && (i != 2 || r.fido2_requested)) || function.rx_pending ||
            (r.configuration && ((!function.rx_active && !function.rx_pending) ||
                                 (function.tx_pending && !function.tx_active &&
                                  (i != 2 || function.tx_bytes == 64))))) {
            return true;
        }
    }
    return false;
}
inline mx_u64_t readable_mask(const Runtime& r) {
    // Controller completions always remain consumable; function TX slots stay
    // queued while their previous chunk owns an IN packet/payload slice.
    const mx_u32_t packet_bytes = r.speed == MxUsbSpeed::High ? 512 : 64;
    mx_u64_t mask = 1;
    for (mx_u32_t i = 0; i < 3; ++i) {
        const auto& f = r.functions[i];
        if (!f.tx_active && f.tx_bytes + MX_FUNCTION_INLINE_BYTES <= (i == 2 ? 64 : packet_bytes))
            mask |= mx_u64_t{1} << (i + 1);
    }
    return mask;
}
} // namespace mochi::usb::gadget

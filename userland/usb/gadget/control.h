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
        if (slot.type == MX_DEVICE_SNAPSHOT) {
            MxDeviceSnapshot snapshot{};
            if (mochi::message_get(slot,&snapshot,slot.type) &&
                r.phase == ControlPhase::Observe && !r.observe_pending &&
                snapshot.id == r.observe_id && snapshot.generation == r.generation) {
                libk::copy_n(libk::slice(r.payload,512,sizeof(snapshot)),
                            libk::as_bytes(&snapshot,sizeof(snapshot)),sizeof(snapshot));
                reply_data(r,sizeof(snapshot));
            }
            continue;
        }
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
    if (r.observe_pending && packet(r,r.observe_trial ? MX_DEVICE_EP0_TRIAL : MX_DEVICE_OBSERVE,
            r.observe_endpoint,0,0,r.observe_trial ? r.trial_mode :
            (r.observe_history ? MX_DEVICE_OBSERVE_CONTROL_HISTORY : 0),&r.observe_id)) {
        r.observe_pending=false;
        progress=true;
    }
    if (r.pending_function && r.acm_control.tx().try_push_msg(MX_FUNCTION_SETUP, r.function_request)) {
        r.pending_function = false;
        progress = true;
    }
    // EP0 replies must remain consumable while a host has stopped polling
    // CDC bulk IN. Port-open class requests may precede its next data read.
    for (mx_u32_t n = 0; n < driver::kBatchLimit && r.acm_control.try_recv(&slot); ++n) {
        progress = true;
        MxFunctionSetup answer{};
        if (!mochi::message_get(slot, &answer, MX_FUNCTION_SETUP_REPLY) ||
            r.phase != ControlPhase::Function || answer.id != r.setup.id ||
            answer.generation != r.function_generation) continue;
        if (answer.id == r.last_out_control.id && !(answer.setup.request_type & 128)) {
            r.last_out_reply_status=answer.status;
            r.last_out_reply_length=answer.length;
        }
        if (answer.status != MX_OK || answer.length > 16) {
            stall(r);
        } else if (r.setup.setup.request_type & 128) {
            libk::copy_n(libk::slice(r.payload, 512, answer.length),
                        libk::as_bytes(answer.data), answer.length);
            reply_data(r, answer.length);
        } else {
            r.phase = ControlPhase::StatusIn;
        }
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
    if (r.demand_pending || r.pending_function || r.observe_pending || (r.configuration && r.configure_next < kFunctionEndpointCount)) {
        return true;
    }
    if (!r.ep0_active && r.phase != ControlPhase::Idle && r.phase != ControlPhase::Function && r.phase != ControlPhase::Observe) {
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

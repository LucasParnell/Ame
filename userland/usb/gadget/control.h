#pragma once
// Controller-neutral EP0 policy. This module borrows explicit ring/payload
// views; the standalone process owns mapping, arenas and service readiness.
// Standard requests stay here, class requests stay in their function process.
#include "mochi/driver.h"
#include "midix/abi/protocols/usb_device.h"
#include "midix/abi/protocols/usb_function.h"
#include "descriptors.h"
namespace mochi::usb::gadget {
enum class ControlPhase : mx_u8_t { Idle, In, OutClass, Function, StatusIn, StatusOut, Stall };
struct Function {
    MxFunctionChunk tx;
    mx_u32_t received, forwarded;
    bool rx_active, rx_pending, tx_pending, tx_active, reset;
};
struct Runtime {
    mochi::SpscEndpoint links[3];
    libk::bytes payload;
    MxDeviceEvent setup;
    MxFunctionSetup function_request;
    Function functions[2];
    mx_u64_t generation, next_id;
    mx_u32_t length, sent, configuration, configure_next;
    mx_u8_t pending_address;
    bool address_pending, ep0_active, zlp, pending_function;
    ControlPhase phase;
    MxUsbSpeed speed;
};
inline bool packet(Runtime& r, mx_u32_t type, mx_u8_t endpoint, mx_u32_t offset, mx_u32_t length,
                   mx_u32_t value = 0) {
    const MxDevicePacket req{(endpoint & 15) ? ++r.next_id : r.setup.id,
                             r.generation,
                             offset,
                             length,
                             0,
                             endpoint,
                             MxUsbTransferType::Control,
                             value,
                             0};
    return r.links[0].tx().try_push_msg(type, req);
}
inline void stall(Runtime& r) {
    // Retain the stall under command-ring pressure like any other EP0 work.
    r.phase = ControlPhase::Stall;
    r.ep0_active = false;
}
inline void reply_data(Runtime& r, mx_u32_t length) {
    r.length = length < r.setup.setup.length ? length : r.setup.setup.length;
    r.sent = 0;
    r.zlp = r.length < r.setup.setup.length && !(r.length % 64);
    r.phase = ControlPhase::In;
    r.ep0_active = false;
}
// Fixed endpoint rows describe function allocation; no controller knowledge.
constexpr mx_u8_t kFunctionEndpoints[] = {1, 0x82, 0x83, 4, 0x85};
constexpr mx_u32_t kFunctionEndpointCount = sizeof(kFunctionEndpoints);
inline bool configure(Runtime& r) {
    bool progress = false;
    while (r.configure_next < kFunctionEndpointCount) {
        const auto ep = kFunctionEndpoints[r.configure_next];
        const MxDevicePacket request{r.next_id + 1,
                                     r.generation,
                                     0,
                                     0,
                                     static_cast<mx_u16_t>(ep == 0x83                    ? 16
                                                           : r.speed == MxUsbSpeed::High ? 512
                                                                                         : 64),
                                     ep,
                                     ep == 0x83 ? MxUsbTransferType::Interrupt
                                                : MxUsbTransferType::Bulk,
                                     0,
                                     0};
        if (!r.links[0].tx().try_push_msg(MX_DEVICE_CONFIGURE, request)) {
            break;
        }
        ++r.next_id;
        ++r.configure_next;
        progress = true;
    }
    return progress;
}
inline void reset_functions(Runtime& r) {
    for (auto& function : r.functions) {
        function = {};
        function.reset = true;
    }
}
inline void function_setup(Runtime& r) {
    r.function_request = {r.setup.id, r.generation, r.setup.setup, MX_OK, 0, {}};
    if (r.setup.setup.length && !(r.setup.setup.request_type & 128)) {
        if (r.setup.setup.length > 16) {
            stall(r);
            return;
        }
        r.phase = ControlPhase::OutClass;
    } else {
        r.pending_function = true;
        r.phase = ControlPhase::Function;
    }
}
inline void setup(Runtime& r, const MxDeviceEvent& event) {
    r.setup = event;
    r.generation = event.generation;
    r.speed = static_cast<MxUsbSpeed>(event.speed);
    r.phase = ControlPhase::Idle;
    r.ep0_active = false;
    r.pending_function = false;
    r.address_pending = false;
    const auto& s = event.setup;
    if ((s.request_type & 0x60) == 0x20 && (s.request_type & 31) == 1 && s.index == 0) {
        function_setup(r);
        return;
    }
    if (s.request_type & 0x60) {
        stall(r);
        return;
    }
    if (s.request == 6 && s.request_type == 0x80) {
        const auto kind = s.value >> 8, index = s.value & 255;
        mx_u32_t length = 0;
        auto buffer = libk::slice(r.payload, 512, 512);
        if (kind == 1 && !index) {
            const auto d = mochi::usb::gadget::device_descriptor();
            libk::copy_n(buffer, d, d.size);
            length = d.size;
        } else if ((kind == 2 || kind == 7) && !index) {
            length = mochi::usb::gadget::configuration(
                buffer, kind == 7 ? r.speed != MxUsbSpeed::High : r.speed == MxUsbSpeed::High,
                kind == 7);
        } else if (kind == 3 && (s.index == 0 || s.index == 0x409)) {
            length = mochi::usb::gadget::string_descriptor(buffer, index);
        } else if (kind == 6 && !index) {
            const auto d = mochi::usb::gadget::qualifier();
            libk::copy_n(buffer, d, d.size);
            length = d.size;
        }
        if (length) {
            reply_data(r, length);
        } else {
            stall(r);
        }
    } else if (s.request == 5 && s.request_type == 0 && !s.index && !s.length && s.value <= 127) {
        r.pending_address = s.value;
        r.address_pending = true;
        r.phase = ControlPhase::StatusIn;
    } else if (s.request == 9 && s.request_type == 0 && !s.index && !s.length && s.value <= 1) {
        r.configuration = s.value;
        r.configure_next = s.value ? 0 : kFunctionEndpointCount;
        reset_functions(r);
        r.phase = ControlPhase::StatusIn;
    } else if (s.request == 8 && s.request_type == 0x80 && !s.index && !s.value && s.length == 1) {
        r.payload.data[512] = r.configuration;
        reply_data(r, 1);
    } else if (s.request == 0 &&
               (s.request_type == 0x80 || s.request_type == 0x81 || s.request_type == 0x82) &&
               !s.value && s.length == 2) {
        r.payload.data[512] = r.payload.data[513] = 0;
        reply_data(r, 2);
    } else if (s.request == 10 && s.request_type == 0x81 && s.index < 3 && !s.value &&
               s.length == 1) {
        r.payload.data[512] = 0;
        reply_data(r, 1);
    } else if (s.request == 11 && s.request_type == 1 && s.index < 3 && !s.value && !s.length) {
        r.phase = ControlPhase::StatusIn;
    } else {
        stall(r);
    }
}
inline void completion(Runtime& r, const MxDeviceEvent& done) {
    if (done.generation != r.generation) {
        return;
    }
    if (!(done.endpoint & 15)) {
        if (done.id != r.setup.id) {
            return;
        }
        r.ep0_active = false;
        if (done.status != MxUsbStatus::Success) {
            stall(r);
            return;
        }
        if (r.phase == ControlPhase::In) {
            r.sent += done.length;
            if (r.sent >= r.length) {
                if (r.zlp && done.length) {
                    r.zlp = false;
                } else {
                    r.phase = ControlPhase::StatusOut;
                }
            }
        } else if (r.phase == ControlPhase::OutClass) {
            if (done.length != r.setup.setup.length) {
                stall(r);
                return;
            }
            r.function_request.length = done.length;
            libk::copy_n(libk::as_bytes(r.function_request.data),
                         libk::slice(r.payload, 0, done.length), done.length);
            r.pending_function = true;
            r.phase = ControlPhase::Function;
        } else if (r.phase == ControlPhase::StatusIn) {
            r.phase = ControlPhase::Idle;
        } else if (r.phase == ControlPhase::StatusOut) {
            r.phase = ControlPhase::Idle;
        }
        return;
    }
    const auto index = (done.endpoint & 15) >= 4 ? 1u : 0u;
    auto& f = r.functions[index];
    if (done.endpoint & 128) {
        f.tx_active = false;
        f.tx_pending = false;
    } else {
        f.rx_active = false;
        f.received = done.length;
        f.forwarded = 0;
        f.rx_pending = done.status == MxUsbStatus::Success && done.length;
    }
}
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
    for (mx_u32_t i = 0; i < 2; ++i) {
        auto& f = r.functions[i];
        if (f.reset) {
            const MxFunctionChunk reset{r.generation, 0, 0, 0, 0, {}};
            if (r.links[i + 1].tx().try_push_msg(MX_FUNCTION_RESET, reset)) {
                f.reset = false;
                progress = true;
            } else {
                continue;
            }
        }
        while (f.rx_pending) {
            MxFunctionChunk chunk{r.generation, 0, 0, static_cast<mx_u8_t>(i ? 4 : 1), 0, {}};
            const auto left = f.received - f.forwarded;
            chunk.length = left < 32 ? left : 32;
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
            if (packet(r, MX_DEVICE_RECEIVE, i ? 4 : 1, 1024 + i * 1024,
                       r.speed == MxUsbSpeed::High ? 512 : 64)) {
                f.rx_active = true;
                progress = true;
            }
        }
        if (f.tx_pending && !f.tx_active && r.configuration) {
            libk::copy_n(libk::slice(r.payload, 1536 + i * 1024, f.tx.length),
                         libk::as_bytes(f.tx.data, f.tx.length), f.tx.length);
            if (packet(r, MX_DEVICE_SEND, i ? 0x85 : 0x82, 1536 + i * 1024, f.tx.length)) {
                f.tx_active = true;
                progress = true;
            }
        }
        if (!f.tx_pending && r.links[i + 1].try_recv(&slot)) {
            progress = true;
            if (slot.type == MX_FUNCTION_SETUP_REPLY) {
                MxFunctionSetup answer{};
                if (mochi::message_get(slot, &answer, slot.type) &&
                    r.phase == ControlPhase::Function && answer.id == r.setup.id &&
                    answer.generation == r.generation) {
                    if (answer.status != MX_OK || answer.length > 16) {
                        stall(r);
                    } else if (r.setup.setup.request_type & 128) {
                        libk::copy_n(libk::slice(r.payload, 512, answer.length),
                                     libk::as_bytes(answer.data, answer.length), answer.length);
                        reply_data(r, answer.length);
                    } else {
                        r.phase = ControlPhase::StatusIn;
                    }
                }
            } else if (mochi::message_get(slot, &f.tx, MX_FUNCTION_TX) && f.tx.length <= 32 &&
                       f.tx.generation == r.generation) {
                f.tx_pending = true;
            }
        }
    }
    return progress;
}
inline bool retry_pending(const Runtime& r) {
    if (r.pending_function || (r.configuration && r.configure_next < kFunctionEndpointCount)) {
        return true;
    }
    if (!r.ep0_active && r.phase != ControlPhase::Idle && r.phase != ControlPhase::Function) {
        return true;
    }
    for (const auto& function : r.functions) {
        if (function.reset || function.rx_pending ||
            (r.configuration && ((!function.rx_active && !function.rx_pending) ||
                                 (function.tx_pending && !function.tx_active)))) {
            return true;
        }
    }
    return false;
}
inline mx_u64_t readable_mask(const Runtime& r) {
    // Controller completions always remain consumable; function TX slots stay
    // queued while their previous chunk owns an IN packet/payload slice.
    return 1u | (!r.functions[0].tx_pending ? 2u : 0u) | (!r.functions[1].tx_pending ? 4u : 0u);
}
} // namespace mochi::usb::gadget

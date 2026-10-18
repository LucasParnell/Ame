#pragma once
// Fixed composition and state rows. The controller sees packet commands only;
// byte-function generations and stream ownership belong to the gadget.
#include "mochi/driver.h"
#include "midix/abi/protocols/usb_device.h"
#include "midix/abi/protocols/usb_function.h"
#include "descriptors.h"
#include "midix/abi/protocols/fido2.h"
namespace mochi::usb::gadget {
enum class ControlPhase : mx_u8_t { Idle, In, OutClass, Function, StatusIn, StatusOut, Stall };
struct Function {
    mx_u64_t rx_id, tx_id;
    mx_u32_t received, forwarded, tx_bytes;
    bool rx_active, rx_pending, tx_pending, tx_active, reset;
};
struct Runtime {
    mochi::SpscEndpoint links[4];
    mochi::Ring fido2_demand;
    libk::bytes payload;
    MxDeviceEvent setup;
    MxFunctionSetup function_request;
    Function functions[3];
    mx_u64_t generation, function_generation, next_id;
    mx_u32_t length, sent, configuration, configure_next;
    mx_u8_t pending_address;
    bool address_pending, ep0_active, zlp, pending_function;
    bool fido2_requested, demand_pending;
    ControlPhase phase;
    MxUsbSpeed speed;
};
inline bool packet(Runtime& r, mx_u32_t type, mx_u8_t endpoint, mx_u32_t offset, mx_u32_t length,
                   mx_u32_t value = 0, mx_u64_t* accepted_id = nullptr) {
    const MxDevicePacket req{(endpoint & 15) ? r.next_id + 1 : r.setup.id,
                             r.generation,
                             offset,
                             length,
                             0,
                             endpoint,
                             MxUsbTransferType::Control,
                             value,
                             0};
    if (!r.links[0].tx().try_push_msg(type, req)) {
        return false;
    }
    if (endpoint & 15) {
        ++r.next_id;
    }
    if (accepted_id) {
        *accepted_id = req.id;
    }
    return true;
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
constexpr mx_u8_t kFunctionEndpoints[] = {1, 0x82, 0x83, 4, 0x85, 6, 0x86};
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
                                                           : (ep & 15) == 6             ? 64
                                                           : r.speed == MxUsbSpeed::High ? 512
                                                                                         : 64),
                                     ep,
                                     ep == 0x83 || (ep & 15) == 6 ? MxUsbTransferType::Interrupt
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
    // Controller identity can remain unchanged across SET_CONFIGURATION.
    // Give the byte functions a fresh incarnation at every lifecycle edge.
    if (r.function_generation == ~mx_u64_t{0}) {
        mx_exit(MX_ERR_OVERFLOW);
    }
    ++r.function_generation;
    if (r.fido2_requested) r.demand_pending = true;
    r.fido2_requested = false;
    for (auto& function : r.functions) {
        function = {};
        function.reset = true;
    }
}
} // namespace mochi::usb::gadget

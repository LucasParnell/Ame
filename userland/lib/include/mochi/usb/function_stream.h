#pragma once
// One retained slot per byte-stream direction. Storage belongs to the service
// arena. A full output ring leaves both the slot and its correlation token owned
// here; the caller must not consume another slot from that direction.
#include "mochi/driver.h"
#include "midix/abi/protocols/usb_function.h"
namespace mochi::usb {
struct PendingFrame {
    MxRingSlot slot;
    bool held;
    bool flush(const mochi::Ring& output) {
        if (!held || !output.try_push_frame(slot)) {
            return false;
        }
        held = false;
        return true;
    }
    bool retain(const MxRingSlot& incoming) {
        if (!mochi::message_valid(incoming) ||
            (incoming.type != MX_FUNCTION_RX && incoming.type != MX_FUNCTION_TX &&
             incoming.type != MX_FUNCTION_RESET)) {
            return false;
        }
        slot = incoming;
        held = true;
        return true;
    }
};
} // namespace mochi::usb

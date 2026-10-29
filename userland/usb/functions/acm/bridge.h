#pragma once
#include "control.h"
#include "mochi/usb/function_stream.h"
namespace mochi::usb::acm {
struct Runtime {
    mochi::SpscEndpoint links[3];
    mochi::usb::PendingFrame pending[2], control_reply;
    mochi::usb::acm::Control control;
};
// EP0 control has its own request/reply rings and retained response. Bulk byte
// backpressure cannot prevent a port-open control reply from reaching gadget.
inline bool service(Runtime& r) {
    bool progress = r.control_reply.flush(r.links[2].tx());
    MxRingSlot control{};
    if (!r.control_reply.held && r.links[2].try_recv(&control)) {
        progress = true;
        MxFunctionSetup request{};
        if (mochi::message_get(control, &request, MX_FUNCTION_SETUP)) {
            r.control.reply(request);
            mochi::message_put(&r.control_reply.slot, MX_FUNCTION_SETUP_REPLY, request);
            r.control_reply.slot.seq = control.seq;
            r.control_reply.held = true;
        }
    }
    for (mx_u32_t from = 0; from < 2; ++from) {
        auto& pending = r.pending[from];
        if (pending.held) {
            progress |= pending.flush(r.links[from ^ 1].tx());
            continue;
        }
        MxRingSlot slot{};
        if (r.links[from].try_recv(&slot)) {
            progress = true;
            (void)pending.retain(slot);
        }
    }
    return progress;
}
} // namespace mochi::usb::acm

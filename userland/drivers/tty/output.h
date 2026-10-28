#pragma once
// The framebuffer terminal is usable even when its optional USB copy stalls.
#include "render.h"
#include "transport.h"
namespace mochi::tty {
inline bool service_shell_output(const terminal::OutputSource& source, SerialMirror& serial,
                                 Render& render, bool live, mx_u64_t now) {
    bool progress = serial.flush(now);
    for (mx_u32_t i = 0; i < 64 && live && serial.pending.empty(); ++i) {
        MxRingSlot slot{};
        if (!source.try_read(&slot)) break;
        render.write(libk::as_bytes(slot.payload, slot.length));
        (void)serial.hold(slot);
        (void)serial.flush(now);
        progress = true;
    }
    return progress;
}
} // namespace mochi::tty

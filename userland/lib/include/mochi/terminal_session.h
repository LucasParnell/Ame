#pragma once
// Optional terminal sessions over existing SPSC rings. Connection generations
// travel with bytes, so delayed output cannot be relabelled on reconnect. USB
// class requests and controller generations remain outside this contract.
#include "mochi/terminal.h"
#include "libk/endian.h"
namespace mochi::terminal {
constexpr mx_u32_t kSessionHeaderBytes = sizeof(mx_u64_t);
constexpr mx_u32_t kSessionRunBytes = MX_TERM_INLINE_BYTES - kSessionHeaderBytes;
inline mx_u64_t session_generation(const MxTermControl& control) {
    return mx_u64_t{control.arg0} | (mx_u64_t{control.arg1} << 32);
}
inline MxTermControl session_control(mx_u32_t code, mx_u64_t generation) {
    return {code, static_cast<mx_u32_t>(generation), static_cast<mx_u32_t>(generation >> 32), 0};
}
inline bool session_bytes(const RingSlot& slot, mx_u32_t type, mx_u64_t* generation,
                          libk::const_bytes* bytes) {
    if (!generation || !bytes || !mochi::message_valid(slot) || slot.type != type ||
        slot.length <= kSessionHeaderBytes) {
        return false;
    }
    const auto value = libk::load_le<mx_u64_t>(slot.payload);
    if (!value) {
        return false;
    }
    *generation = value;
    *bytes = libk::slice(libk::as_bytes(slot.payload), kSessionHeaderBytes,
                         slot.length - kSessionHeaderBytes);
    return true;
}
struct SessionSink : Ring {
    // Returns the accepted prefix. Callers retain a cursor over any suffix;
    // this never reports failure after secretly publishing part of a run.
    mx_u32_t try_write(mx_u32_t type, mx_u64_t generation, libk::const_bytes bytes) const {
        if (!generation || !bytes.data || !bytes.size ||
            (type != MX_TERM_SESSION_INPUT_BYTES && type != MX_TERM_SESSION_OUTPUT_BYTES)) {
            return 0;
        }
        const auto count = bytes.size < kSessionRunBytes ? bytes.size : kSessionRunBytes;
        mx_u8_t wire[MX_TERM_INLINE_BYTES];
        libk::write_le<mx_u64_t>(libk::as_bytes(wire), generation);
        libk::copy_n(libk::slice(libk::as_bytes(wire), kSessionHeaderBytes, count), bytes, count);
        return try_push(type, wire, kSessionHeaderBytes + count) ? count : 0;
    }
};
} // namespace mochi::terminal

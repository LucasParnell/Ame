#pragma once
// mochi/message.h — typed messages over the ring-slot inline payload.
//
// A ring slot carries at most MX_RING_INLINE_PAYLOAD (48) bytes inline. Almost
// every service passes one fixed-size POD through it; this is the single
// encode/decode pair for that, so no service hand-rolls a byte-copy loop.
//
// Data-oriented: the slot is the storage, `T` is the view, and both are trivially
// copyable — no allocation, no construction, no hidden state. A message that
// cannot fit one slot belongs in a shared arena, not here (the static_assert
// says so at compile time rather than truncating at run time).

#include "libk/bytes.h"
#include "midix/abi/ring.h"

namespace mochi {

inline bool message_valid(const MxRingSlot& slot) {
    return slot.length <= MX_RING_INLINE_PAYLOAD;
}

// Validate the wire representation once for every typed sender and decoder.
template <typename T>
constexpr mx_u32_t message_size() {
    static_assert(__is_trivially_copyable(T), "inline messages must be trivially copyable");
    static_assert(sizeof(T) <= MX_RING_INLINE_PAYLOAD,
                  "message exceeds the inline ring payload; put it in an arena");
    return static_cast<mx_u32_t>(sizeof(T));
}

// Encode `msg` into `slot` as a message of `type`. The slot's inline payload is
// written in full, including a zeroed tail; seq starts at zero. Ring publication
// stamps its own sequence. Callers must initialise all transmitted fields and
// padding within T; these helpers copy its object representation.
template <typename T>
inline bool message_put(MxRingSlot* slot, mx_u32_t type, const T& msg) {
    constexpr mx_u32_t size = message_size<T>();
    if (!slot) return false;
    slot->type = type;
    slot->length = size;
    slot->seq = 0;
    const auto payload = libk::as_bytes(slot->payload);
    (void)libk::copy_n(payload, libk::as_bytes(&msg, size), size);
    (void)libk::zero(libk::slice(payload, size, payload.size - size));
    return true;
}

// Decode `slot` into `out`. `expect` is the required opcode, or 0 to accept any
// opcode. Returns false and leaves `out` untouched if the opcode does not match
// or the length is shorter than T or larger than the inline payload.
template <typename T>
inline bool message_get(const MxRingSlot& slot, T* out, mx_u32_t expect = 0) {
    constexpr mx_u32_t size = message_size<T>();
    if (!out) return false;
    if (expect != 0 && slot.type != expect) return false;
    if (slot.length < size || !message_valid(slot)) return false;
    return libk::copy_n(libk::as_bytes(out, size), libk::as_bytes(slot.payload), size);
}

} // namespace mochi

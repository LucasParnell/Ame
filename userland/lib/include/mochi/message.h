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

#include "midix/abi/ring.h"

namespace mochi {

// Encode `msg` into `slot` as a message of `type`. The slot's inline payload is
// written in full; `seq` is left for the ring producer to stamp.
template <typename T>
inline bool message_put(MxRingSlot* slot, mx_u32_t type, const T& msg) {
    static_assert(sizeof(T) <= MX_RING_INLINE_PAYLOAD,
                  "message exceeds the inline ring payload; put it in an arena");
    if (!slot) return false;
    slot->type = type;
    slot->length = static_cast<mx_u32_t>(sizeof(T));
    slot->seq = 0;
    const auto* src = reinterpret_cast<const mx_u8_t*>(static_cast<const void*>(&msg));
    for (mx_u32_t i = 0; i < sizeof(T); ++i) slot->payload[i] = src[i];
    return true;
}

// Decode `slot` into `out`. `expect` is the required opcode, or 0 to accept any
// opcode. Returns false and leaves `out` untouched if the opcode does not match
// or the inline payload is shorter than the message.
template <typename T>
inline bool message_get(const MxRingSlot& slot, T* out, mx_u32_t expect = 0) {
    static_assert(sizeof(T) <= MX_RING_INLINE_PAYLOAD,
                  "message exceeds the inline ring payload; put it in an arena");
    if (!out) return false;
    if (expect != 0 && slot.type != expect) return false;
    if (slot.length < sizeof(T)) return false;
    auto* dst = reinterpret_cast<mx_u8_t*>(static_cast<void*>(out));
    for (mx_u32_t i = 0; i < sizeof(T); ++i) dst[i] = slot.payload[i];
    return true;
}

} // namespace mochi

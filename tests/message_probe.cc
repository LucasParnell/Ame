// tests/message_probe.cc — host test for the typed ring-slot message codec
// (mochi/message.h). The codec is pure and host-compilable: it names the slot
// type and copies a fixed-size POD in and out of the inline payload.
#include "mochi/message.h"

#include <cassert>
#include <cstring>

namespace {

struct Ping {
    mx_u32_t seq;
    mx_u32_t payload;
};
static_assert(sizeof(Ping) <= MX_RING_INLINE_PAYLOAD, "test message must fit one slot");

} // namespace

int main() {
    MxRingSlot slot{};
    const Ping sent{7u, 0xDEADBEEFu};

    // Round trip.
    assert(mochi::message_put(&slot, 0x1234u, sent));
    assert(slot.type == 0x1234u);
    assert(slot.length == sizeof(Ping));

    Ping got{};
    assert(mochi::message_get(slot, &got, 0x1234u));
    assert(got.seq == 7u && got.payload == 0xDEADBEEFu);

    // A wrong expected opcode is rejected and leaves the out-param untouched.
    Ping other{99u, 99u};
    assert(!mochi::message_get(slot, &other, 0x9999u));
    assert(other.seq == 99u && other.payload == 99u);

    // Any-opcode decode (expect == 0) accepts it.
    assert(mochi::message_get(slot, &got, 0u));

    // A short inline payload is rejected rather than reading past it.
    MxRingSlot short_slot = slot;
    short_slot.length = sizeof(Ping) - 1u;
    assert(!mochi::message_get(short_slot, &got, 0x1234u));

    // Null guards.
    assert(!mochi::message_put(nullptr, 1u, sent));
    assert(!mochi::message_get(slot, static_cast<Ping*>(nullptr), 0x1234u));

    // The encoder writes the whole payload, not just the low bytes.
    MxRingSlot dirty{};
    dirty.payload[sizeof(Ping)] = 0xAAu;
    assert(mochi::message_put(&dirty, 0x5u, sent));
    assert(dirty.payload[sizeof(Ping)] == 0xAAu);

    return 0;
}

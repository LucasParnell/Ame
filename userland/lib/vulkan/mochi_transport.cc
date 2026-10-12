// userland/lib/vulkan/mochi_transport.cc — mochi-ring Transport binding.

#include "mochi_transport.h"

namespace dango::vk {
namespace {

bool mochi_push(void* ctx, const MxVkFrame& slot) {
    auto* ship = static_cast<MochiShip*>(ctx);
    return ship && ship->request_ring &&
           ring_try_push_frame(ship->request_ring, ship->order, slot, ship->request_doorbell);
}

bool mochi_pop(void* ctx, MxVkFrame* out) {
    auto* ship = static_cast<MochiShip*>(ctx);
    return ship && ship->reply_ring && ring_pop(ship->reply_ring, ship->order, out);
}

// The blocking half of the futex-shaped ring protocol: publish the sleep flag,
// wait on the reply doorbell, then clear it. The caller retries pop() after a
// true return, so this never consumes a slot on its own.
bool mochi_wait(void* ctx) {
    auto* ship = static_cast<MochiShip*>(ctx);
    if (!ship || !ship->reply_ring) return false;
    return ring_wait_readable(ship->reply_ring, ship->order, ship->reply_doorbell);
}

} // namespace

Transport make_mochi_transport(MochiShip* ship) {
    Transport transport{};
    transport.ctx = ship;
    transport.push = &mochi_push;
    transport.pop = &mochi_pop;
    transport.wait = &mochi_wait;
    transport.map_arena = nullptr; // no native shared-memory attach in V0
    return transport;
}

} // namespace dango::vk

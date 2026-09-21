// userland/lib/vulkan/mochi_transport.cc — mochi-ring Transport binding.

#include "mochi_transport.h"

namespace dango::vk {
namespace {

bool mochi_push(void* ctx, const MxVkFrame& slot) {
    auto* ship = static_cast<MochiShip*>(ctx);
    return ship && ship->request_ring &&
           ring_try_push(ship->request_ring, ship->order, slot.type, slot.payload, slot.length,
                         ship->request_doorbell);
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
    const mx_u64_t counter = sys_doorbell_read(ship->reply_doorbell);
    ship->reply_ring->flags.doorbell_counter_snapshot = counter;
    __atomic_store_n(&ship->reply_ring->flags.receiver_about_to_sleep, 1u, __ATOMIC_RELEASE);
    const mx_i64_t rc = sys_wait_doorbell(ship->reply_doorbell, counter);
    __atomic_store_n(&ship->reply_ring->flags.receiver_about_to_sleep, 0u, __ATOMIC_RELEASE);
    return rc >= 0;
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

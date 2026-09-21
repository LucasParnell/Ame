#pragma once
// userland/lib/vulkan/mochi_transport.h — the mochi-ring adapter for the native
// Vulkan session client.
//
// dango/vulkan.h is transport-abstract; this is the one concrete binding a
// native service uses: a request ring (client -> service), a reply ring
// (service -> client) and their two doorbells, exactly the shape
// drv::gpu::linux::Endpoint already uses. It reuses the mochi ring push/pop and
// the futex-shaped wait, so there is no second implementation of the ring
// protocol. The arena is not mappable from a native service in V0 (no shared
// memory attach yet), so map_arena returns null and a session opened over this
// transport reports no ARENA_READY.

#include "dango/vulkan.h"
#include "mochi/ring.h"

namespace dango::vk {

// The two rings and their doorbells, borrowed from the service endpoint.
struct MochiShip {
    RingPage* request_ring = nullptr;
    RingPage* reply_ring = nullptr;
    mx_u32_t order = MX_DEFAULT_RING_ORDER;
    CapHandle request_doorbell = kNullCap; // client NOTIFY
    CapHandle reply_doorbell = kNullCap;   // client WAIT
};

// Build a Transport that drives `ship`. The returned value borrows `ship`, so
// the caller keeps it alive for the session's lifetime.
Transport make_mochi_transport(MochiShip* ship);

} // namespace dango::vk

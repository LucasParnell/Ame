#pragma once
// Small client-side helper for the v0 nameservice protocol.
// Caller supplies its own SPSC endpoint rings; this helper does not allocate or
// ask the kernel to copy data. Full dynamic endpoint allocation is a later
// nameservice policy, not a kernel feature.

#include "libk/str.h"
#include "mochi/endpoint.h"
#include "midix/abi/protocols/nameservice.h"

namespace mochi::nameservice {

static inline uint32_t copy_name(char* dst, uint32_t dst_len, const char* src) {
    return static_cast<uint32_t>(libk::str::copy_into(dst, dst_len, src));
}

static inline bool try_resolve_request(const SpscEndpoint& ns, const char* name) {
    MxNsResolveRequest req{};
    copy_name(req.name, sizeof(req.name), name);
    return ns.try_send(MSG_NS_RESOLVE, &req, sizeof(req));
}

static inline mx_status_t wait_resolve_reply(const SpscEndpoint& ns, MxEndpointDesc* out) {
    RingSlot slot{};
    for (;;) {
        if (!ns.wait_recv(&slot)) return MX_ERR_DEAD;
        MxEndpointDesc endpoint{};
        if (message_get(slot, &endpoint, MSG_NS_RESOLVE_REPLY)) {
            if (out) *out = endpoint;
            return MX_OK;
        }
        mx_status_t status = MX_ERR_NOT_FOUND;
        if (message_get(slot, &status, MSG_NS_ERROR)) return status;
    }
}

} // namespace mochi::nameservice

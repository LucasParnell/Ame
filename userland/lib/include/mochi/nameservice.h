#pragma once
// Small client-side helper for the v0 nameservice protocol.
// Caller supplies its own SPSC endpoint rings; this helper does not allocate or
// ask the kernel to copy data. Full dynamic endpoint allocation is a later
// nameservice policy, not a kernel feature.

#include "mochi/endpoint.h"
#include "midix/abi/protocols/nameservice.h"

namespace mochi::nameservice {

static inline uint32_t copy_name(char* dst, uint32_t dst_len, const char* src) {
    if (!dst || dst_len == 0) return 0;
    uint32_t n = 0;
    if (src) {
        while (n + 1 < dst_len && src[n]) {
            dst[n] = src[n];
            ++n;
        }
    }
    dst[n] = '\0';
    return n;
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
        if (slot.type == MSG_NS_RESOLVE_REPLY && slot.length >= sizeof(MxEndpointDesc)) {
            if (out) {
                for (uint32_t i = 0; i < sizeof(MxEndpointDesc); ++i) {
                    reinterpret_cast<uint8_t*>(out)[i] = slot.payload[i];
                }
            }
            return MX_OK;
        }
        if (slot.type == MSG_NS_ERROR && slot.length >= sizeof(mx_status_t)) {
            mx_status_t status = MX_ERR_NOT_FOUND;
            for (uint32_t i = 0; i < sizeof(status); ++i) {
                reinterpret_cast<uint8_t*>(&status)[i] = slot.payload[i];
            }
            return status;
        }
    }
}

} // namespace mochi::nameservice

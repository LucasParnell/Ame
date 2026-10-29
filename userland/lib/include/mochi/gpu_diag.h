#pragma once
#include "mochi/driver.h"
#include "midix/abi/protocols/gpu_diag.h"

namespace mochi::gpu_diag {
constexpr mx_u64_t kControlVa = 0x33000000;
constexpr mx_u64_t kDisplayVa = 0x33040000;
constexpr mx_u64_t kPixelsVa = 0x34000000;
constexpr mx_u64_t kSnapshotVa = 0x35000000;

inline bool exchange(const SpscEndpoint& link, mx_u32_t op, const ame_gpu_diag_request& request,
                     ame_gpu_diag_reply& reply, mx_u64_t deadline) {
    if (!link.valid() || !link.tx().push_bounded(op, &request, sizeof(request), deadline)) {
        return false;
    }
    MxRingSlot slot{};
    while (driver::receive(link, slot, deadline)) {
        if (message_get(slot, &reply, AME_GPU_DIAG_REPLY) && reply.id == request.id) {
            return true;
        }
    }
    return false;
}
} // namespace mochi::gpu_diag

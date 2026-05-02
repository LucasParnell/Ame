#pragma once

#include <cstdint>

namespace dango {

constexpr uint32_t VM_CHANNEL_HOST_TO_GUEST_RING = 0;
constexpr uint32_t VM_CHANNEL_GUEST_TO_HOST_RING = 1;
constexpr uint32_t VM_CHANNEL_HOST_TO_GUEST_DOORBELL = 2;
constexpr uint32_t VM_CHANNEL_GUEST_TO_HOST_DOORBELL = 3;

} // namespace dango

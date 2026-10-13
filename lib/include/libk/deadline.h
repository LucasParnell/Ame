#pragma once
// Pure absolute-time selection. Zero means no timer; clock reads and waits
// belong to the caller. This matches the doorbell ABI without importing it.
#include <cstdint>
namespace libk {
inline constexpr uint64_t earlier_deadline(uint64_t current, uint64_t candidate) {
    return !candidate ? current : !current || candidate < current ? candidate : current;
}
} // namespace libk

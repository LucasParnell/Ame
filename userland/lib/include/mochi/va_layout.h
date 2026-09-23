#pragma once
// mochi/va_layout.h — the userland service VA plan.
//
// Every service is its own address space and maps its capability pages at fixed
// VAs it chooses. Those choices belong in one place, not scattered as literals
// per service, for two reasons:
//
//   * the whole plan is visible at once, so a reader (and a reviewer) can see
//     which windows exist and that they cannot collide; and
//   * the windows are *precomputed*: Planner lays a service's windows out by
//     bumping a cursor, so disjointness is structural rather than asserted by
//     hand. Adding a window is one reserve() line, and there is no literal
//     address to mistype.
//
// These are mapping *targets* in a private address space, not device addresses.
// Each service contributes one section below.

#include "libk/integer.h"
#include "midix/abi/ring.h"

namespace mochi::va {

constexpr uint64_t kPage = 0x1000;

// Compile-time window planner: reserve(size) returns the next page-aligned
// window and advances past it, so reserved windows never overlap.
class Planner {
public:
    constexpr explicit Planner(uint64_t base) : next_(base) {}
    constexpr uint64_t reserve(uint64_t size, uint64_t align = kPage) {
        const uint64_t at = libk::align_up(next_, align);
        next_ = at + size;
        return at;
    }

private:
    uint64_t next_;
};

// --- io.console ------------------------------------------------------------
namespace console {

constexpr uint32_t kLogRingOrder  = 6;          // slot-count log2 per log ring
constexpr uint64_t kLogRingStride = 0x1'0000ull;
constexpr uint64_t kLogRingCount  = 32;         // == ConsoleBootConfig::kMaxRings
constexpr uint64_t kLogRingSpan   = kLogRingStride * kLogRingCount;
static_assert(kLogRingStride >= mx_ring_page_bytes(kLogRingOrder),
              "log ring VA stride must hold one ring page");

// The config page sits apart from the UART/ring windows.
constexpr uint64_t kConfigBase = 0x3000'0000ull;

struct Windows {
    uint64_t uart;
    uint64_t log_ring_base;
    uint64_t mirror_ring;

    constexpr uint64_t log_ring(uint32_t i) const {
        return log_ring_base + i * kLogRingStride;
    }
};

constexpr Windows plan() {
    Planner p{0x2000'0000ull};
    Windows w{};
    w.uart          = p.reserve(kPage);
    w.log_ring_base = p.reserve(kLogRingSpan);
    w.mirror_ring   = p.reserve(kPage);
    return w;
}

constexpr Windows kWindows = plan();

} // namespace console

} // namespace mochi::va

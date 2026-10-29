#pragma once

#include "libk/framebuffer.h"

namespace midix::tegra_dc {

// Tegra210 DC0 register indices (32-bit words, not byte offsets).
inline constexpr std::uint64_t kDc0Base = 0x54200000;
inline constexpr unsigned kStateAccess = 0x040;
inline constexpr unsigned kWindowHeader = 0x042;
inline constexpr unsigned kOptions = 0x700;
inline constexpr unsigned kColorDepth = 0x703;
inline constexpr unsigned kSize = 0x705;
inline constexpr unsigned kPrescaledSize = 0x706;
inline constexpr unsigned kStride = 0x70a;
inline constexpr unsigned kBufferStart = 0x800;
inline constexpr unsigned kOffsetH = 0x806;
inline constexpr unsigned kOffsetV = 0x808;
inline constexpr unsigned kSurfaceKind = 0x80b;
inline constexpr std::uint32_t kWindowA = 1u << 4;
inline constexpr std::uint32_t kReadActive = 1;
inline constexpr std::uint32_t kWindowEnabled = 1u << 30;

struct Window {
    std::uint32_t options, format, size, prescaled_size, stride;
    std::uint32_t address, offset_h, offset_v, surface_kind;
};

constexpr libk::Framebuffer32 framebuffer(const Window& w, std::uint64_t ram_base,
                                         std::uint64_t ram_size) {
    const libk::Framebuffer32 fb{w.address, w.size & 0x1fff, (w.size >> 16) & 0x1fff};
    if (!(w.options & kWindowEnabled) || (w.options & (1u << 4)) ||
        (w.format != 0xc && w.format != 0xd) || w.surface_kind != 0 ||
        w.offset_h || w.offset_v || (w.stride & 0xffff) != fb.width * 4 ||
        (w.prescaled_size & 0x7fff) != fb.width * 4 ||
        ((w.prescaled_size >> 16) & 0x1fff) != fb.height ||
        !libk::framebuffer_fits(fb, ram_base, ram_size) ||
        !libk::framebuffer_fits(fb, 0, std::uint64_t{1} << 32)) {
        return {};
    }
    return fb;
}

#if defined(__aarch64__)
// Only call while DC0/Host1x clocks are active and physical MMIO is accessible.
// Restore both selectors; do not change scanout or guess a framebuffer address.
inline libk::Framebuffer32 read_active(std::uint64_t ram_base, std::uint64_t ram_size) {
    auto* dc = reinterpret_cast<volatile std::uint32_t*>(kDc0Base);
    const auto access = dc[kStateAccess];
    const auto header = dc[kWindowHeader];
    dc[kWindowHeader] = kWindowA;
    dc[kStateAccess] = access | kReadActive;
    asm volatile("dsb sy" ::: "memory");
    const Window window{dc[kOptions], dc[kColorDepth], dc[kSize], dc[kPrescaledSize],
                        dc[kStride], dc[kBufferStart], dc[kOffsetH], dc[kOffsetV],
                        dc[kSurfaceKind]};
    dc[kStateAccess] = access;
    dc[kWindowHeader] = header;
    asm volatile("dsb sy" ::: "memory");
    return framebuffer(window, ram_base, ram_size);
}
#endif

} // namespace midix::tegra_dc

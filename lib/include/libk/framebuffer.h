#pragma once

#include <cstdint>

namespace libk {

// Tightly packed 32-bit pixels. Validate against the owning memory range before
// use; padded, tiled and other pixel formats require a different layout.
struct Framebuffer32 {
    std::uint64_t address;
    std::uint32_t width;
    std::uint32_t height;
};

constexpr bool framebuffer_fits(Framebuffer32 fb, std::uint64_t base,
                                std::uint64_t size) {
    if (!fb.address || (fb.address & 3) || !fb.width || !fb.height ||
        size > ~std::uint64_t{0} - base || fb.address < base || fb.address - base >= size) {
        return false;
    }
    // Divide before multiplying: even malicious dimensions cannot wrap.
    return fb.width <= (size - (fb.address - base)) / sizeof(std::uint32_t) / fb.height;
}

// Requires a validated framebuffer and directly accessible address. Cache
// maintenance/barriers belong to the caller's memory-mapping policy.
inline void framebuffer_fill(Framebuffer32 fb, std::uint32_t color) {
    auto* pixels = reinterpret_cast<volatile std::uint32_t*>(fb.address);
    const auto count = std::uint64_t{fb.width} * fb.height;
    for (std::uint64_t i = 0; i < count; ++i) {
        pixels[i] = color;
    }
}

} // namespace libk

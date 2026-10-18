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

// Pixel storage is normal memory (including Normal NC), not device registers.
// Keep stores non-volatile so the compiler can combine adjacent pixels. The
// owner must publish writes with the cache maintenance/barriers required by
// its mapping before scanout or another device consumes them.
inline void framebuffer_fill_span(std::uint32_t* pixels, std::uint64_t count,
                                  std::uint32_t color) {
    for (std::uint64_t i = 0; i < count; ++i) pixels[i] = color;
}

template <std::uint32_t Scale>
inline void framebuffer_mono_full_span(std::uint32_t* pixels, std::uint8_t bits,
                                       std::uint32_t foreground, std::uint32_t background) {
    for (std::uint32_t gx = 0; gx < 8; ++gx) {
        const auto color = bits & (0x80u >> gx) ? foreground : background;
        for (std::uint32_t sx = 0; sx < Scale; ++sx) pixels[gx * Scale + sx] = color;
    }
}

// Expand one high-bit-first monochrome row into a contiguous pixel span.
// Requires 1 <= scale <= 8 and count <= 8 * scale, already clipped by caller.
inline void framebuffer_mono_span(std::uint32_t* pixels, std::uint8_t bits,
                                  std::uint32_t count, std::uint32_t scale,
                                  std::uint32_t foreground, std::uint32_t background) {
    // Most terminal cells are complete. Fixed scales let the compiler unroll
    // eight font bits into adjacent stores without dynamic per-pixel loops.
    if (count == 8 * scale) {
        switch (scale) {
        case 1: framebuffer_mono_full_span<1>(pixels, bits, foreground, background); return;
        case 2: framebuffer_mono_full_span<2>(pixels, bits, foreground, background); return;
        case 3: framebuffer_mono_full_span<3>(pixels, bits, foreground, background); return;
        case 4: framebuffer_mono_full_span<4>(pixels, bits, foreground, background); return;
        case 5: framebuffer_mono_full_span<5>(pixels, bits, foreground, background); return;
        case 6: framebuffer_mono_full_span<6>(pixels, bits, foreground, background); return;
        case 7: framebuffer_mono_full_span<7>(pixels, bits, foreground, background); return;
        case 8: framebuffer_mono_full_span<8>(pixels, bits, foreground, background); return;
        }
    }
    std::uint32_t at = 0;
    for (std::uint32_t gx = 0; at < count; ++gx) {
        const auto color = bits & (0x80u >> gx) ? foreground : background;
        const auto run = count - at < scale ? count - at : scale;
        framebuffer_fill_span(pixels + at, run, color);
        at += run;
    }
}

// Requires a validated framebuffer and directly accessible address.
inline void framebuffer_fill(Framebuffer32 fb, std::uint32_t color) {
    auto* pixels = reinterpret_cast<std::uint32_t*>(fb.address);
    const auto count = std::uint64_t{fb.width} * fb.height;
    framebuffer_fill_span(pixels, count, color);
}

} // namespace libk

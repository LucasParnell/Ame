#include "libk/framebuffer.h"
#include "midix/hw/tegra_dc.h"
#include <cassert>

int main() {
    using libk::Framebuffer32;
    using libk::framebuffer_fits;
    constexpr std::uint64_t base = 0x80000000;
    constexpr std::uint64_t size = 0x80000000;
    constexpr Framebuffer32 fb{0xf5a00000, 720, 1280};
    assert(framebuffer_fits(fb, base, size));
    assert(!framebuffer_fits({0, 720, 1280}, base, size));
    assert(!framebuffer_fits({base + 1, 720, 1280}, base, size));
    assert(!framebuffer_fits({base - 4, 720, 1280}, base, size));
    assert(!framebuffer_fits({base, 0, 1280}, base, size));
    assert(!framebuffer_fits({base, 720, 0}, base, size));
    assert(!framebuffer_fits({base + size - 4, 2, 1}, base, size));
    assert(!framebuffer_fits({base, ~std::uint32_t{0}, ~std::uint32_t{0}}, base, size));
    assert(!framebuffer_fits({~std::uint64_t{3}, 1, 1}, ~std::uint64_t{7}, 16));
    assert(framebuffer_fits({base + size - 4, 1, 1}, base, size));

    using namespace midix::tegra_dc;
    const Window live{kWindowEnabled, 0xc, (1280u << 16) | 720,
                      (1280u << 16) | (720 * 4), 720 * 4, 0xf5a00000, 0, 0, 0};
    assert(framebuffer(live, base, size).address == fb.address);
    assert(!framebuffer({}, base, size).address); // disabled/reset controller
    // Each rejected layout used to risk writing into unrelated RAM.
    const struct { std::uint32_t Window::*field; std::uint32_t value; } invalid[] = {
        {&Window::options, 0}, {&Window::options, kWindowEnabled | (1u << 4)},
        {&Window::format, 6}, {&Window::size, 0}, {&Window::prescaled_size, 0},
        {&Window::stride, 4096}, {&Window::address, 0},
        {&Window::address, 0xfffff000}, {&Window::offset_h, 4},
        {&Window::offset_v, 1}, {&Window::surface_kind, 2},
    };
    for (const auto& mutation : invalid) {
        auto window = live;
        window.*mutation.field = mutation.value;
        assert(!framebuffer(window, base, size).address);
    }
    alignas(8) std::uint32_t pixels[10]{};
    const Framebuffer32 target{reinterpret_cast<std::uintptr_t>(&pixels[1]), 4, 2};
    assert(framebuffer_fits(target, target.address, 8 * sizeof(pixels[0])));
    libk::framebuffer_fill(target, 0xffff00ff);
    assert(pixels[0] == 0 && pixels[9] == 0);
    for (unsigned i = 1; i < 9; ++i) assert(pixels[i] == 0xffff00ff);
}

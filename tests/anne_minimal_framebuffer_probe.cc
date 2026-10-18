#include "framebuffer.h"
#include "timing.h"
#include <cassert>
#include <vector>

int main() {
    using namespace anne::minimal;
    static_assert(kCounterHz == 1920000 && kLedHoldTicks == 0);
    static_assert(elapsed(100, 100, kLedHoldTicks)); // LED stages never add a dwell
    static_assert(!elapsed(100, 5759999 + 100, 5760000));
    static_assert(elapsed(100, 5760000 + 100, 5760000));
    static_assert(!elapsed(100, 100 + kBlinkTicks - 1, kBlinkTicks));
    static_assert(elapsed(100, 100 + kBlinkTicks, kBlinkTicks));
    static_assert(elapsed(~std::uint64_t{0} - 5, 4, 10)); // counter wrap
    Registers r{1, 5u << 3, static_cast<unsigned>(kLogoBase), 270,
                0, 0, 269, 2279, 1, 1079, 2279};
    auto b = validate(r);
    assert(b.address == kLogoBase && b.width == 1080 && b.height == 2280);
    auto reject = [&](Registers bad) { assert(!validate(bad).address); };
    for (unsigned bit : {1, 8, 9, 12, 17}) { auto bad = r; bad.control |= 1u << bit; reject(bad); }
    auto bad = r; bad.channel |= 4; reject(bad);
    bad = r; bad.address = 0x480000; reject(bad);
    bad = r; bad.address = kLogoBase + kLogoSize - 16; reject(bad);
    bad = r; bad.x1 = 270; reject(bad); // source wider than its stride
    bad = r; bad.y1 = 65535; reject(bad);
    bad = r; bad.stride |= 1u << 16; reject(bad);
    bad = r; bad.x0 = 270; reject(bad);
    bad = r; bad.ldi = 0; reject(bad);
    // Cropped DMA rectangles have an already-offset DATA_ADDR, unlike panel geometry.
    auto crop = r; crop.x0 = 3; crop.x1 = 34; crop.y0 = 10; crop.y1 = 73;
    b = validate(crop); assert(b.width == 128 && b.height == 64);
    assert(pixel(0, 0x00ff00) == 0x07e0 && pixel(5, 0x00ff00) == 0xff00ff00);
    for (unsigned format = 0; format <= 6; ++format) {
        r.control = format << 3;
        r.x1 = format < 5 ? 134 : 269;
        r.stride = format < 5 ? 135 : 270;
        assert(validate(r).address);
        const unsigned bpp = format < 5 ? 2 : 4;
        // Render into bounded host storage; padding and guard bytes must survive.
        std::vector<unsigned char> pixels(128 * 80 * bpp + 32, 0xaa);
        Buffer fb{reinterpret_cast<std::uint64_t>(pixels.data()), 120, 80, 128 * bpp, format};
        paint(fb, 1);
        for (unsigned y = 0; y < 80; ++y)
            for (unsigned x = 120 * bpp; x < 128 * bpp; ++x) assert(pixels[y * 128 * bpp + x] == 0xaa);
        for (unsigned i = 128 * 80 * bpp; i < pixels.size(); ++i) assert(pixels[i] == 0xaa);
        const auto* last = pixels.data() + 79 * 128 * bpp;
        if (bpp == 4) assert(*reinterpret_cast<const std::uint32_t*>(last) == pixel(format, 0x00ff00));
        else assert(*reinterpret_cast<const std::uint16_t*>(last) == pixel(format, 0x00ff00));
    }
}

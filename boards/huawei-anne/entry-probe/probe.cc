#include "display.h"

// No allocator, UART, interrupts, DTB parser, BSS, or static constructors.
// Only read display registers; preserve all firmware display configuration.
extern "C" void anne_entry_probe(unsigned long entry_el) {
    using namespace anne::display;
    auto* dma = reinterpret_cast<volatile std::uint32_t*>(kDss + kRdmaD3);
    auto* ldi = reinterpret_cast<volatile std::uint32_t*>(kDss + kLdi0);
    const Registers state{dma[0xd4/4], dma[0x1c/4], dma[0x60/4], dma[0x64/4],
                          dma[0], dma[4/4], dma[0x10/4], dma[0x14/4],
                          ldi[0x24/4], ldi[8/4], ldi[0x14/4]};
    const auto buffer = validate(state);
    if (!buffer.address) return;
    const std::uint32_t color = entry_el == 1 ? 0xff00ff00 : 0xffff00ff;
    for (unsigned y = 0; y < buffer.height; ++y) {
        const auto address = buffer.address + std::uint64_t{y} * buffer.stride;
        if (buffer.bytes_per_pixel == 4) {
            auto* row = reinterpret_cast<volatile std::uint32_t*>(address);
            for (unsigned x = 0; x < buffer.width; ++x) row[x] = color;
        } else {
            auto* row = reinterpret_cast<volatile std::uint16_t*>(address);
            for (unsigned x = 0; x < buffer.width; ++x) row[x] = entry_el == 1 ? 0x07e0 : 0xf81f;
        }
    }
    asm volatile("dsb sy" ::: "memory");
}

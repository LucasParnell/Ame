#include "anko/hw_info.h"
#include "midix/hw/tegra_dc.h"

// No UART, SD, allocator, DTB parser or static constructors in this path.
// Magenta proves ATF entered this BL33 payload and DC0 scanout was writable.
extern "C" bool handoff_probe() {
    const auto& board = platform::get_board_fallback();
    const auto fb = midix::tegra_dc::read_active(board.ram_base, board.ram_size);
    if (fb.address) {
        libk::framebuffer_fill(fb, 0xffff00ffu);
        asm volatile("dsb sy" ::: "memory");
    }
    return fb.address != 0;
}

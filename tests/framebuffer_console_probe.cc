#include "anko/framebuffer_console.h"
#include "midix/font/vga8x16.h"
#include <cassert>
#include <cstring>

int main() {
    constexpr u32 width = 128, height = 128, guard = 16, untouched = 0x12345678;
    static u32 storage[width * height + 2 * guard];
    for (auto& pixel : storage) pixel = untouched;
    const u64 address = reinterpret_cast<u64>(storage + guard);
    console::FramebufferConsole screen;
    const libk::Framebuffer32 fb{address, width, height};
    assert(screen.bind(fb, address, width * height * 4, {16, 32, 80, 64, 1}));
    assert(screen.columns() == 10 && screen.rows() == 4);
    screen.draw_text(16, 8, 1, "ANKO", 0xffffffff);
    const auto* glyph = midix::font::vga8x16_glyph('A');
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 8; ++x)
            assert(storage[guard + (8 + y) * width + 16 + x] ==
                   (glyph[y] & (0x80 >> x) ? 0xffffffff : screen.kBackground));
    screen.write("abcdefghij\nK\n", 13);
    assert(screen.cell(0, 9) == 'j' && screen.cell(1, 0) == 'K' && screen.cell(2, 0) == ' ');
    assert(screen.bind(fb, address, width * height * 4, {16, 32, 80, 64, 1}));
    screen.write("one\ntwo\nthree\nfour\nfive", 23);
    assert(screen.cell(0, 0) == 't' && screen.cell(1, 0) == 't');
    assert(screen.cell(2, 0) == 'f' && screen.cell(3, 0) == 'f');
    screen.write("\rA\bB\tC", 7);
    assert(screen.cell(3, 0) == 'B' && screen.cell(3, 4) == 'C');
    u64 dirty; u32 bytes;
    assert(screen.take_dirty(dirty, bytes));
    assert(dirty == address + 32 * width * 4 && bytes == 64 * width * 4);
    assert(!screen.take_dirty(dirty, bytes));
    for (u32 i = 0; i < guard; ++i) {
        assert(storage[i] == untouched && storage[guard + width * height + i] == untouched);
    }
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            if (x < 16 || x >= 96 || (y < 8 || (y >= 24 && y < 32) || y >= 96))
                assert(storage[guard + y * width + x] == untouched);
    assert(!screen.bind(fb, address, width * height * 4, {120, 32, 16, 64, 1}));
    assert(!screen.active() && screen.columns() == 0 && screen.rows() == 0);
    screen.write("unsafe\n", 7);
    assert(!screen.bind(fb, address, width * height * 4 - 1, {16, 32, 80, 64, 1}));
    assert(!screen.bind(fb, address, width * height * 4, {16, 32, 80, 64, 0}));
    assert(screen.bind(fb, address, width * height * 4, {16, 32, 80, 64, 1},
                       console::FramebufferConsole::Overflow::Wrap));
    screen.write("one\ntwo\nthree\nfour", 18);
    assert(screen.take_dirty(dirty, bytes)); // Discard initial viewport damage.
    screen.write("\nfive", 5);
    // Page flip on rollover: the text area is cleared, not wrapped over, so no
    // stale line survives above the newest one.
    assert(screen.cell(0, 0) == 'f' && screen.cell(0, 1) == 'i');
    for (u32 row = 1; row < 4; ++row)
        for (u32 col = 0; col < screen.columns(); ++col) assert(screen.cell(row, col) == ' ');
    assert(screen.take_dirty(dirty, bytes));
    assert(dirty == address + 32 * width * 4 && bytes == 64 * width * 4);
    // The banner above y=24 was painted earlier; everything else outside the
    // text viewport must still be untouched.
    for (u32 y = 24; y < height; ++y)
        if (y < 32 || y >= 96)
            for (u32 x = 0; x < width; ++x) assert(storage[guard + y * width + x] == untouched);
    static u32 phone[1080 * 2280];
    for (auto& pixel : phone) pixel = untouched;
    const u64 phone_address = reinterpret_cast<u64>(phone);
    assert(screen.bind({phone_address, 1080, 2280}, phone_address, sizeof(phone), {64, 224, 952, 1960, 2}));
    assert(screen.columns() == 59 && screen.rows() == 61);
    screen.draw_text(64, 144, 3, "ANKO EL1", 0xff7ee6a8);
    for (u32 y = 0; y < 128; ++y)
        for (u32 x = 0; x < 1080; ++x) assert(phone[y * 1080 + x] == untouched);
}

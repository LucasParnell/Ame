#include "../userland/drivers/tty/render.h"
#include "../userland/drivers/framebuffer/raster.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <string>

struct Fixture {
    struct { MxRingPage page{}; MxRingSlot slots[4]{}; } commands;
    mochi::tty::Render render{};
    std::vector<char> cells;
    std::vector<std::string> lines;
    unsigned x = 0, y = 0, copies = 0, draws = 0;
    std::vector<uint32_t> pixels, expected;
    mochi::framebuffer::Raster raster;
    Fixture(unsigned cols, unsigned rows, unsigned scale = 1)
        : cells(rows * cols), lines(rows, std::string(cols, ' ')),
          pixels(std::max(256u, cols * 8 * scale + 6) * 256, 0x12345678),
          expected(pixels), raster{pixels.data(), std::max(256u, cols * 8 * scale + 6), 256} {
        render.fb.cmd = {&commands.page, 2, 1};
        render.cells = cells.data(); render.cols = cols; render.rows = rows;
        render.scale = scale; render.cell_w = 8 * scale; render.cell_h = 16 * scale;
        render.origin_x = 3; render.origin_y = 7;
        render.init();
    }
    void newline() {
        x = 0;
        if (++y == lines.size()) {
            y = lines.size() - 1;
            lines.erase(lines.begin());
            lines.emplace_back(render.cols, ' ');
        }
    }
    void reference(char ch) {
        if (ch == '\n') newline();
        else if (ch == '\r') x = 0;
        else if (ch == '\b') { if (x) lines[y][--x] = ' '; }
        else if (ch == '\t') {
            const auto n = 8 - x % 8;
            for (unsigned i = 0; i < n; ++i) reference(' ');
        } else {
            if (x == render.cols) newline();
            lines[y][x++] = ch >= 32 && ch <= 126 ? ch : '?';
        }
    }
    void write(const std::string& bytes) {
        for (auto ch : bytes) reference(ch);
        render.write(libk::as_bytes(bytes.data(), bytes.size()));
        assert(render.x == x && render.y == y);
        for (unsigned row = 0; row < render.rows; ++row)
            assert(!std::memcmp(render.line(row), lines[row].data(), render.cols));
    }
    bool consume() {
        MxRingSlot slot{};
        if (!render.fb.cmd.try_pop(&slot)) return false;
        switch (slot.type) {
        case FB_CLEAR: {
            uint32_t color;
            assert(mochi::message_get(slot, &color, FB_CLEAR));
            raster.clear(color);
            break;
        }
        case FB_COPY_RECT: {
            FbCopyRectCmd cmd;
            assert(mochi::message_get(slot, &cmd, FB_COPY_RECT));
            raster.copy_rect(cmd); ++copies;
            break;
        }
        case FB_DRAW_TEXT: {
            FbDrawTextCmd cmd;
            assert(mochi::message_get(slot, &cmd, FB_DRAW_TEXT));
            raster.draw_text(cmd); ++draws;
            break;
        }
        case FB_FILL_RECT: {
            FbFillRectCmd cmd;
            assert(mochi::message_get(slot, &cmd, FB_FILL_RECT));
            raster.fill_rect(cmd.x, cmd.y, cmd.width, cmd.height, cmd.color);
            break;
        }
        case FB_PRESENT: break;
        default: assert(false);
        }
        return true;
    }
    void finish() {
        for (unsigned i = 0; render.pending(); ++i) {
            assert(i < 1000);
            render.flush(1);
            consume();
        }
        while (consume()) {}
        mochi::framebuffer::Raster oracle{expected.data(), raster.width, raster.height};
        oracle.clear(render.kBg);
        for (unsigned row = 0; row < render.rows; ++row) for (unsigned col = 0; col < render.cols;) {
            FbDrawTextCmd cmd{};
            cmd.x = render.origin_x + col * render.cell_w;
            cmd.y = render.origin_y + row * render.cell_h;
            cmd.fg = render.kFg; cmd.bg = render.kBg;
            cmd.len = std::min(render.cols - col, kFbDrawTextMaxChars);
            cmd.scale_x = cmd.scale_y = render.scale;
            std::memcpy(cmd.text, lines[row].data() + col, cmd.len);
            oracle.draw_text(cmd);
            col += cmd.len;
        }
        oracle.fill_rect(render.origin_x + std::min(x, render.cols - 1) * render.cell_w,
                         render.origin_y + y * render.cell_h + render.cell_h - 2,
                         render.cell_w, 2, render.kFg);
        assert(pixels == expected); // actual pixels, including cursor and untouched margins
    }
};
int main() {
    Fixture f(5, 3);
    f.write("aaaaa\nbbbbb\nccccc");
    f.finish();
    f.draws = f.copies = 0;
    f.write("\n");
    f.finish();
    assert(f.copies == 1 && f.draws == 2); // exposed row plus old cursor cell
    f.draws = f.copies = 0;
    f.write("\n\n");
    f.finish();
    assert(f.copies == 1 && f.draws == 3); // two exposed rows plus old cursor cell
    f.copies = 0;
    f.write("\n\n\n\n\n");
    f.finish();
    assert(f.copies == 0); // a whole-screen scroll discards all old pixels

    Fixture wide(63, 3);
    wide.finish();
    for (unsigned i = 0; i < 100; ++i) {
        wide.write(std::string(63, 'a' + i % 26));
        wide.render.flush(2); // stop in the middle of a multi-command dirty row
        wide.consume();
        wide.write("\n");
        if (i % 7 == 0) wide.finish();
    }
    wide.finish();

    // Interleave writes, partial command admission and partial consumption.
    // This covers scrolling while old draws or the cursor remain in the ring.
    uint32_t rng = 0x12345678;
    for (unsigned rows : {1u, 2u, 5u}) for (unsigned scale : {1u, 2u}) {
        Fixture test(7, rows, scale);
        for (unsigned i = 0; i < 2500; ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const char bytes[] = {'a', 'b', 'c', ' ', '\r', '\n', '\b', '\t', '\1'};
            test.write(std::string(1, bytes[rng % sizeof(bytes)]));
            if (rng & 1) test.render.flush(1 + rng % 4);
            if (rng & 2) test.consume();
            if (rng % 41 == 0) test.finish();
        }
        test.finish();
    }
}

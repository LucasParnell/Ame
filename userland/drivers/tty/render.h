#pragma once
// A character grid and bounded framebuffer publisher. Single-owner state
// lives in the service arena; rows rotate on scroll instead of being copied.
// Dirty column ranges collapse a byte burst into a small set of commands.
#include "mochi/framebuffer.h"
#include "libk/str.h"
namespace mochi::tty {
struct Render {
    static constexpr mx_u32_t kRows = 256, kFg = 0xe6f1ff, kBg = 0x001020;
    mochi::framebuffer::Client fb;
    char* cells;
    mx_u32_t cols, rows, cell_w, cell_h, origin_x, origin_y, scale;
    mx_u32_t x, y, first_row, scan_row, dirty_count;
    mx_u16_t dirty_first[kRows], dirty_end[kRows];
    mx_u32_t cursor_row, cursor_col;
    bool clear_pending, cursor_pending, cursor_visible, present_pending;
    char* line(mx_u32_t row) const {
        return cells + ((first_row + row) % rows) * cols;
    }
    void dirty(mx_u32_t row, mx_u32_t begin, mx_u32_t end) {
        if (dirty_first[row] == dirty_end[row]) {
            ++dirty_count;
            dirty_first[row] = begin;
            dirty_end[row] = end;
        } else {
            if (begin < dirty_first[row]) {
                dirty_first[row] = begin;
            }
            if (end > dirty_end[row]) {
                dirty_end[row] = end;
            }
        }
        if (row < scan_row) {
            scan_row = row;
        }
        present_pending = true;
    }
    void init() {
        libk::fill(libk::as_bytes(cells, rows * cols), ' ');
        clear_pending = cursor_pending = present_pending = true;
    }
    void hide_cursor() {
        if (cursor_visible) {
            dirty(cursor_row, cursor_col, cursor_col + 1);
        }
        cursor_visible = false;
        cursor_pending = true;
    }
    void newline() {
        x = 0;
        if (y + 1 < rows) {
            ++y;
        } else {
            first_row = (first_row + 1) % rows;
            libk::fill(libk::as_bytes(line(rows - 1), cols), ' ');
            // Logical row contents changed; repaint once after the burst.
            for (mx_u32_t row = 0; row < rows; ++row) {
                dirty(row, 0, cols);
            }
        }
    }
    void character(char ch) {
        if (ch == '\n') {
            newline();
        } else if (ch == '\r') {
            x = 0;
        } else if (ch == '\b') {
            if (x) {
                --x;
                line(y)[x] = ' ';
                dirty(y, x, x + 1);
            }
        } else if (ch == '\t') {
            const auto count = 8 - x % 8;
            for (mx_u32_t i = 0; i < count; ++i) {
                character(' ');
            }
        } else {
            if (x == cols) {
                newline();
            }
            line(y)[x] = libk::str::is_ascii_printable(ch) ? ch : '?';
            dirty(y, x, x + 1);
            ++x;
        }
    }
    void write(libk::const_bytes bytes) {
        if (!bytes.size) {
            return;
        }
        hide_cursor();
        mx_u32_t at = 0;
        while (at < bytes.size) {
            const auto ch = static_cast<char>(bytes.data[at]);
            if (!libk::str::is_ascii_printable(ch)) {
                character(ch);
                ++at;
                continue;
            }
            if (x == cols) {
                newline();
            }
            mx_u32_t count = 0;
            while (at + count < bytes.size && count < cols - x &&
                   libk::str::is_ascii_printable(static_cast<char>(bytes.data[at + count]))) {
                ++count;
            }
            libk::copy_n(libk::as_bytes(line(y) + x, cols - x), libk::slice(bytes, at, count),
                         count);
            dirty(y, x, x + count);
            x += count;
            at += count;
        }
    }
    bool pending() const {
        return clear_pending || dirty_count || cursor_pending || present_pending;
    }
    bool flush(mx_u32_t budget = 64) {
        bool progress = false;
        while (budget-- && pending()) {
            if (clear_pending) {
                const mx_u32_t color = kBg;
                if (!fb.try_send(FB_CLEAR, &color, sizeof(color))) {
                    break;
                }
                clear_pending = false;
            } else if (dirty_count) {
                while (scan_row < rows && dirty_first[scan_row] == dirty_end[scan_row]) {
                    ++scan_row;
                }
                if (scan_row == rows) {
                    break; // all dirty rows must have been admitted
                }
                const auto col = dirty_first[scan_row];
                const uint32_t left = dirty_end[scan_row] - col;
                const auto count = left < kFbDrawTextMaxChars ? left : kFbDrawTextMaxChars;
                FbDrawTextCmd command{};
                command.x = origin_x + col * cell_w;
                command.y = origin_y + scan_row * cell_h;
                command.fg = kFg;
                command.bg = kBg;
                command.len = count;
                command.scale_x = command.scale_y = scale;
                libk::copy_n(libk::as_bytes(command.text),
                             libk::as_bytes(line(scan_row) + col, count), count);
                if (!fb.try_send(FB_DRAW_TEXT, &command, sizeof(command))) {
                    break;
                }
                dirty_first[scan_row] += count;
                if (dirty_first[scan_row] == dirty_end[scan_row]) {
                    --dirty_count;
                }
            } else if (cursor_pending) {
                const auto row = y < rows ? y : rows - 1;
                const auto col = x < cols ? x : cols - 1;
                const FbFillRectCmd command{origin_x + col * cell_w,
                                            origin_y + row * cell_h + cell_h - 2, cell_w, 2, kFg};
                if (!fb.try_send(FB_FILL_RECT, &command, sizeof(command))) {
                    break;
                }
                cursor_row = row;
                cursor_col = col;
                cursor_pending = false;
                cursor_visible = true;
                present_pending = true;
            } else {
                if (!fb.try_send(FB_PRESENT, nullptr, 0)) {
                    break;
                }
                present_pending = false;
            }
            progress = true;
        }
        return progress;
    }
};
} // namespace mochi::tty

// userland/drivers/tty/main.cc — tty.fb0 text terminal service (M3 / M7).
//
// Renders a text grid onto io.framebuffer. Receives log lines from io.console
// (non-blocking write endpoint) and keyboard input from ui.input (M7).
//
// Rules:
//  - Does NOT read kernel log rings directly.
//  - Does NOT hold framebuffer pixel memory.
//  - Only io.console sends log records here; tty.fb0 does not poll log rings.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/framebuffer.h"
#include "mochi/input.h"
#include "mochi/terminal.h"

constexpr uint64_t kVaBootInfo = 0x1000'0000ull;
constexpr uint64_t kVaLogRing  = 0x1800'0000ull;
constexpr uint64_t kVaFbCmdRing = 0x1900'0000ull;
[[maybe_unused]] constexpr uint64_t kVaFbReplyRing = 0x1902'0000ull; // keep clear of order-10 cmd ring
constexpr uint64_t kVaMirrorRing = 0x1904'0000ull;
constexpr uint64_t kVaInputRing = 0x1906'0000ull; // avoid order-10 mirror ring overlap
constexpr uint64_t kVaShellInputRing = 0x1908'0000ull;  // tty -> msh
constexpr uint64_t kVaShellOutputRing = 0x190A'0000ull; // msh -> tty

// Text grid dimensions: cover the full 1024×768 ramfb with 8×16 cells.
// Keeping the terminal at 80×25 made boot logs hit the scroll path almost
// immediately, which can back-pressure the framebuffer command ring.
constexpr uint32_t kCols = 128;
constexpr uint32_t kRows = 48;
constexpr uint32_t kCellW = 8;
constexpr uint32_t kCellH = 16;
constexpr uint32_t kRingOrder = MX_DEFAULT_RING_ORDER;
constexpr uint32_t kFg = 0xE6F1FF;
constexpr uint32_t kBg = 0x001020;

struct TtyState {
    uint32_t cursor_x;
    uint32_t cursor_y;
    // V0: store character grid as plain ASCII for log mirror.
    char     cells[kRows][kCols];
};

static TtyState g_tty{};
static mochi::framebuffer::Client g_fb{};
static mochi::terminal::OutputSource g_mirror{};
static mochi::input::EventSource g_input{};
struct ShellInputSink {
    RingPage* ring;
    uint32_t order;
    CapHandle doorbell;

    bool valid() const {
        return ring && doorbell != kNullCap;
    }

    bool write_input_byte_spinning(char ch, uint32_t max_yields) const {
        return valid() && ring_send_spinning(ring, order, MX_TERM_INPUT_BYTES,
                                             &ch, 1, doorbell, max_yields);
    }
};

static ShellInputSink g_shell_input{};
static mochi::terminal::OutputSource g_shell_output{};
static bool g_shell_attached = false;
static bool g_render_suppressed = false;
static bool g_cursor_visible = false;
static uint32_t g_cursor_row = 0;
static uint32_t g_cursor_col = 0;

constexpr uint32_t HID_KEY_RIGHT = 0x4Fu;
constexpr uint32_t HID_KEY_LEFT = 0x50u;
constexpr uint32_t HID_KEY_DOWN = 0x51u;
constexpr uint32_t HID_KEY_UP = 0x52u;
constexpr uint32_t HID_KEY_HOME = 0x4Au;
constexpr uint32_t HID_KEY_END = 0x4Du;
constexpr uint32_t HID_KEY_DELETE = 0x4Cu;
constexpr uint32_t HID_KEY_ESCAPE = 0x29u;

// Input is silently dropped until init logs "system ready" (scanned out of the
// mirror stream below). The break-glass override is three consecutive ESC
// presses — any other key resets the streak.
static constexpr char kReadyMarker[] = "system ready";
static constexpr uint32_t kReadyMarkerLen = sizeof(kReadyMarker) - 1;
static constexpr uint32_t kEscBypassCount = 3;
static bool     g_input_unlocked = false;
static uint32_t g_ready_match_pos = 0;
static uint32_t g_esc_streak = 0;

static uint32_t slen(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static bool fb_send(uint32_t type, const void* data, uint32_t len) {
    // Framebuffer commands are the terminal's source of truth.  Dropping one
    // command leaves the visible text permanently truncated until a later full
    // redraw, which looks like a framebuffer stall.  Back-pressure tty.fb0
    // instead and let io.framebuffer drain at its own pace.
    return g_fb.send_spinning(type, data, len, 0xFFFF'FFFFu);
}


static void fb_clear_screen() {
    if (g_render_suppressed) return;
    const uint32_t color = kBg;
    (void)fb_send(FB_CLEAR, &color, sizeof(color));
}

static void fb_fill_cell(uint32_t row, uint32_t col) {
    if (g_render_suppressed) return;
    FbFillRectCmd cmd{};
    cmd.x = col * kCellW;
    cmd.y = row * kCellH;
    cmd.width = kCellW;
    cmd.height = kCellH;
    cmd.color = kBg;
    (void)fb_send(FB_FILL_RECT, &cmd, sizeof(cmd));
}

static void fb_copy_rect(uint32_t src_x, uint32_t src_y, uint32_t dst_x,
                         uint32_t dst_y, uint32_t width, uint32_t height) {
    if (g_render_suppressed) return;
    FbCopyRectCmd cmd{};
    cmd.src_x = src_x;
    cmd.src_y = src_y;
    cmd.dst_x = dst_x;
    cmd.dst_y = dst_y;
    cmd.width = width;
    cmd.height = height;
    (void)fb_send(FB_COPY_RECT, &cmd, sizeof(cmd));
}

static void fb_draw_text_run(uint32_t row, uint32_t col, const char* data, uint32_t len);

static void fb_draw_cell(uint32_t row, uint32_t col) {
    if (g_render_suppressed) return;
    const char ch = g_tty.cells[row][col];
    if (ch == ' ') {
        fb_fill_cell(row, col);
        return;
    }

    fb_draw_text_run(row, col, &ch, 1);
}

static void fb_draw_text_run(uint32_t row, uint32_t col, const char* data, uint32_t len) {
    if (!data || len == 0) return;
    if (g_render_suppressed) return;

    uint32_t off = 0;
    while (off < len) {
        const uint32_t remaining = len - off;
        const uint32_t chunk = remaining > kFbDrawTextMaxChars
            ? static_cast<uint32_t>(kFbDrawTextMaxChars)
            : remaining;

        FbDrawTextCmd cmd{};
        cmd.x = (col + off) * kCellW;
        cmd.y = row * kCellH;
        cmd.fg = kFg;
        cmd.bg = kBg;
        cmd.len = static_cast<uint8_t>(chunk);
        cmd.scale_x = 1;
        cmd.scale_y = 1;
        for (uint32_t i = 0; i < chunk; ++i) {
            const char ch = data[off + i];
            cmd.text[i] = (ch >= 32 && ch <= 126) ? ch : '?';
        }
        (void)fb_send(FB_DRAW_TEXT, &cmd, sizeof(cmd));
        off += chunk;
    }
}

static void tty_cursor_hide() {
    if (!g_cursor_visible) return;
    if (g_cursor_row < kRows && g_cursor_col < kCols) {
        fb_draw_cell(g_cursor_row, g_cursor_col);
    }
    g_cursor_visible = false;
}

static void tty_cursor_show() {
    if (g_render_suppressed) return;
    const uint32_t row = g_tty.cursor_y < kRows ? g_tty.cursor_y : (kRows - 1u);
    const uint32_t col = g_tty.cursor_x < kCols ? g_tty.cursor_x : (kCols - 1u);
    FbFillRectCmd cmd{};
    cmd.x = col * kCellW;
    cmd.y = row * kCellH + (kCellH - 2u);
    cmd.width = kCellW;
    cmd.height = 2;
    cmd.color = kFg;
    (void)fb_send(FB_FILL_RECT, &cmd, sizeof(cmd));
    g_cursor_row = row;
    g_cursor_col = col;
    g_cursor_visible = true;
}

static void tty_redraw_all() {
    tty_cursor_hide();
    fb_clear_screen();
    for (uint32_t r = 0; r < kRows; ++r) {
        fb_draw_text_run(r, 0, g_tty.cells[r], kCols);
    }
    (void)fb_send(FB_PRESENT, nullptr, 0);
}

static void tty_scroll_pixels_up_one_row() {
    fb_copy_rect(0, kCellH, 0, 0, kCols * kCellW, (kRows - 1u) * kCellH);

    FbFillRectCmd clear{};
    clear.x = 0;
    clear.y = (kRows - 1u) * kCellH;
    clear.width = kCols * kCellW;
    clear.height = kCellH;
    clear.color = kBg;
    (void)fb_send(FB_FILL_RECT, &clear, sizeof(clear));
}

static void fb_present() {
    if (g_render_suppressed) return;
    (void)fb_send(FB_PRESENT, nullptr, 0);
}

static void tty_newline() {
    g_tty.cursor_x = 0;
    if (g_tty.cursor_y + 1 < kRows) {
        ++g_tty.cursor_y;
    } else {
        // Scroll: shift rows up.
        for (uint32_t r = 0; r + 1 < kRows; ++r) {
            for (uint32_t c = 0; c < kCols; ++c) {
                g_tty.cells[r][c] = g_tty.cells[r + 1][c];
            }
        }
        for (uint32_t c = 0; c < kCols; ++c) {
            g_tty.cells[kRows - 1][c] = ' ';
        }
        tty_scroll_pixels_up_one_row();
    }
}

static void tty_write_char(char ch) {
    tty_cursor_hide();
    if (ch == '\n') {
        tty_newline();
        return;
    }
    if (ch == '\r') {
        g_tty.cursor_x = 0;
        return;
    }
    if (ch == '\b') {
        if (g_tty.cursor_x > 0) {
            --g_tty.cursor_x;
            g_tty.cells[g_tty.cursor_y][g_tty.cursor_x] = ' ';
            fb_draw_cell(g_tty.cursor_y, g_tty.cursor_x);
        }
        return;
    }
    if (ch == '\t') {
        const uint32_t next = (g_tty.cursor_x + 8u) & ~7u;
        while (g_tty.cursor_x < next && g_tty.cursor_x < kCols) {
            tty_write_char(' ');
        }
        return;
    }
    if (g_tty.cursor_x >= kCols) tty_newline();
    const uint32_t row = g_tty.cursor_y;
    const uint32_t col = g_tty.cursor_x++;
    g_tty.cells[row][col] = (ch >= 32 && ch <= 126) ? ch : '?';
    fb_draw_cell(row, col);
}

static bool tty_is_printable(char ch) {
    return ch >= 32 && ch <= 126;
}

static void tty_write_printable_run(const char* s, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        if (g_tty.cursor_x >= kCols) tty_newline();

        const uint32_t row = g_tty.cursor_y;
        const uint32_t col = g_tty.cursor_x;
        const uint32_t room = kCols - col;
        uint32_t chunk = len - off;
        if (chunk > room) chunk = room;
        if (chunk > kFbDrawTextMaxChars) {
            chunk = static_cast<uint32_t>(kFbDrawTextMaxChars);
        }

        for (uint32_t i = 0; i < chunk; ++i) {
            g_tty.cells[row][col + i] = s[off + i];
        }
        fb_draw_text_run(row, col, s + off, chunk);
        g_tty.cursor_x += chunk;
        off += chunk;
    }
}

static void tty_write(const char* s, uint32_t len) {
    tty_cursor_hide();
    uint32_t i = 0;
    while (i < len) {
        if (tty_is_printable(s[i])) {
            uint32_t run = 0;
            while ((i + run) < len && tty_is_printable(s[i + run])) ++run;
            tty_write_printable_run(s + i, run);
            i += run;
        } else {
            tty_write_char(s[i]);
            ++i;
        }
    }
}

static void scan_ready_marker(const char* data, uint32_t len) {
    if (g_input_unlocked || !data) return;
    for (uint32_t i = 0; i < len; ++i) {
        const char b = data[i];
        if (b == kReadyMarker[g_ready_match_pos]) {
            ++g_ready_match_pos;
            if (g_ready_match_pos == kReadyMarkerLen) {
                g_input_unlocked = true;
                g_ready_match_pos = 0;
                return;
            }
        } else {
            // "system ready" has no internal periodicity, so the only retry
            // worth doing is treating the mismatching byte as a fresh start.
            g_ready_match_pos = (b == kReadyMarker[0]) ? 1u : 0u;
        }
    }
}

static bool handle_mirror_slot(const RingSlot& slot) {
    const char* bytes = reinterpret_cast<const char*>(slot.payload);
    scan_ready_marker(bytes, slot.length);
    tty_write(bytes, slot.length);
    return true;
}

static bool send_shell_input_char(char ch) {
    if (!g_shell_attached || !g_shell_input.valid()) return false;
    return g_shell_input.write_input_byte_spinning(ch, 4096);
}

static bool send_shell_input_bytes(const char* bytes, uint32_t len) {
    if (!bytes || len == 0) return false;
    for (uint32_t i = 0; i < len; ++i) {
        if (!send_shell_input_char(bytes[i])) return false;
    }
    return true;
}

static bool handle_input_event(const InputEvent& event) {
    if (!g_input_unlocked) {
        // Pre-ready gate: count ESC streaks, drop everything else. INPUT_TEXT
        // events for the ESC code (0x1B) are not currently produced by the
        // existing key-press path, so the HID keycode is the only trigger.
        if (event.type == INPUT_KEY_DOWN) {
            if (event.keycode == HID_KEY_ESCAPE) {
                ++g_esc_streak;
                if (g_esc_streak >= kEscBypassCount) {
                    g_input_unlocked = true;
                    g_esc_streak = 0;
                }
            } else {
                g_esc_streak = 0;
            }
        }
        return false;
    }

    if (event.type == INPUT_KEY_DOWN) {
        const uint32_t key = event.keycode;
        if (key == HID_KEY_UP) return send_shell_input_bytes("\x1b[A", 3);
        if (key == HID_KEY_DOWN) return send_shell_input_bytes("\x1b[B", 3);
        if (key == HID_KEY_RIGHT) return send_shell_input_bytes("\x1b[C", 3);
        if (key == HID_KEY_LEFT) return send_shell_input_bytes("\x1b[D", 3);
        if (key == HID_KEY_HOME) return send_shell_input_bytes("\x1b[H", 3);
        if (key == HID_KEY_END) return send_shell_input_bytes("\x1b[F", 3);
        if (key == HID_KEY_DELETE) return send_shell_input_bytes("\x1b[3~", 4);
    }

    if (event.type != INPUT_TEXT) return false;
    const uint32_t cp = event.unicode;
    if (cp == '\n' || cp == '\r' || cp == '\b' || cp == '\t' ||
        cp == 1 || cp == 2 || cp == 3 || cp == 4 || cp == 5 || cp == 6 ||
        cp == 11 || cp == 12 || cp == 14 || cp == 16 || cp == 21 ||
        (cp >= 32 && cp <= 126)) {
        const char ch = static_cast<char>(cp == '\r' ? '\n' : cp);
        if (send_shell_input_char(ch)) return true;
        tty_write_char(ch);
        return true;
    }
    return false;
}

static bool drain_input(uint32_t max_slots) {
    if (!g_input.ring) return false;
    bool any = false;
    InputEvent event{};
    uint32_t drained = 0;
    while (drained < max_slots && g_input.try_read(&event)) {
        any = handle_input_event(event) || any;
        ++drained;
    }
    return any;
}

static bool drain_mirror(uint32_t max_slots) {
    if (!g_mirror.ring) return false;
    bool any = false;
    RingSlot slot{};
    uint32_t drained = 0;
    while (drained < max_slots && g_mirror.try_read(&slot)) {
        any = handle_mirror_slot(slot) || any;
        ++drained;
    }
    return any;
}

static bool drain_shell_output(uint32_t max_slots) {
    if (!g_shell_output.ring) return false;
    // Hold shell terminal output until init has logged "system ready". Without
    // this, the msh banner/prompt can interleave with the tail of the boot log
    // on first paint. The marker scan in handle_mirror_slot flips
    // g_input_unlocked at the same moment input gating ends. msh back-pressures
    // on its output ring in the meantime — exactly the behaviour we want.
    if (!g_input_unlocked) return false;
    bool any = false;
    RingSlot slot{};
    uint32_t drained = 0;
    while (drained < max_slots && g_shell_output.try_read(&slot)) {
        tty_write(reinterpret_cast<const char*>(slot.payload), slot.length);
        any = true;
        ++drained;
    }
    return any;
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);

    (void)mochi::log_map_from_boot(boot, kVaLogRing, "tty.fb0");

    const MxBootEntry* fb_cmd =
        mochi::boot::find_type(boot, "fb.cmd.ring", MX_BOOT_ENTRY_RING);
    RingPage* fb_cmd_ring = nullptr;
    RingPage* fb_reply_ring = nullptr;
    uint32_t fb_cmd_order = kRingOrder;
    uint32_t fb_reply_order = kRingOrder;
    if (fb_cmd && fb_cmd->handle != MX_NULL_HANDLE &&
        mx_map_page(fb_cmd->handle, kVaFbCmdRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        fb_cmd_ring = reinterpret_cast<RingPage*>(kVaFbCmdRing);
        fb_cmd_order = fb_cmd->aux0
            ? static_cast<uint32_t>(fb_cmd->aux0)
            : kRingOrder;
    }
    const MxBootEntry* fb_reply =
        mochi::boot::find_type(boot, "fb.reply.ring", MX_BOOT_ENTRY_RING);
    if (fb_reply && fb_reply->handle != MX_NULL_HANDLE &&
        mx_map_page(fb_reply->handle, kVaFbReplyRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        fb_reply_ring = reinterpret_cast<RingPage*>(kVaFbReplyRing);
        fb_reply_order = fb_reply->aux0
            ? static_cast<uint32_t>(fb_reply->aux0)
            : kRingOrder;
    }
    g_fb = {
        fb_cmd_ring,
        fb_reply_ring,
        fb_cmd_order,
        fb_reply_order,
        mochi::boot::handle(boot, "fb.cmd.doorbell"),
        mochi::boot::handle(boot, "fb.reply.doorbell"),
    };

    const MxBootEntry* mirror =
        mochi::boot::find_type(boot, "console.mirror.ring", MX_BOOT_ENTRY_RING);
    if (mirror && mirror->handle != MX_NULL_HANDLE &&
        mx_map_page(mirror->handle, kVaMirrorRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_mirror.ring = reinterpret_cast<RingPage*>(kVaMirrorRing);
        g_mirror.order = mirror->aux0
            ? static_cast<uint32_t>(mirror->aux0)
            : kRingOrder;
    }
    g_mirror.doorbell = mochi::boot::handle(boot, "console.mirror.doorbell");

    const mx_handle_t input = mochi::boot::handle(boot, "input.tty.ring");
    if (input != MX_NULL_HANDLE &&
        mx_map_page(input, kVaInputRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_input.ring = reinterpret_cast<RingPage*>(kVaInputRing);
    }
    g_input.order = kRingOrder;
    g_input.doorbell = mochi::boot::handle(boot, "input.tty.doorbell");

    const MxBootEntry* shell_input =
        mochi::boot::find_type(boot, "term.input.ring", MX_BOOT_ENTRY_RING);
    if (shell_input && shell_input->handle != MX_NULL_HANDLE &&
        mx_map_page(shell_input->handle, kVaShellInputRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_shell_input.ring = reinterpret_cast<RingPage*>(kVaShellInputRing);
        g_shell_input.order = shell_input->aux0
            ? static_cast<uint32_t>(shell_input->aux0)
            : kRingOrder;
        g_shell_input.doorbell = mochi::boot::handle(boot, "term.input.doorbell");
    }

    const MxBootEntry* shell_output =
        mochi::boot::find_type(boot, "term.output.ring", MX_BOOT_ENTRY_RING);
    if (shell_output && shell_output->handle != MX_NULL_HANDLE &&
        mx_map_page(shell_output->handle, kVaShellOutputRing,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_shell_output.ring = reinterpret_cast<RingPage*>(kVaShellOutputRing);
        g_shell_output.order = shell_output->aux0
            ? static_cast<uint32_t>(shell_output->aux0)
            : kRingOrder;
        g_shell_output.doorbell = mochi::boot::handle(boot, "term.output.doorbell");
    }
    g_shell_attached = g_shell_input.valid() && g_shell_output.valid();
    if (g_shell_attached) {
        mochi::log_raw_marker("tty.fb0: shell terminal attached\n");
    } else {
        mochi::log_raw_marker("tty.fb0: shell terminal not attached; local echo fallback\n");
    }

    for (uint32_t r = 0; r < kRows; ++r) {
        for (uint32_t c = 0; c < kCols; ++c) {
            g_tty.cells[r][c] = ' ';
        }
    }

    fb_clear_screen();
    mochi::log_raw_marker("tty.fb0: ready\n");
    const char* ready = "tty.fb0: Midix framebuffer terminal ready\n";
    tty_write(ready, slen(ready));
    tty_cursor_show();
    fb_present();

    while (true) {
        const bool any_input = drain_input(64);
        const bool any_shell = drain_shell_output(64);
        const uint32_t mirror_depth = g_mirror.depth();
        const bool bulk_mirror = mirror_depth > 8;
        g_render_suppressed = bulk_mirror;
        const uint32_t mirror_budget = bulk_mirror ? mirror_depth : 4;
        const bool any_mirror = drain_mirror(mirror_budget);
        g_render_suppressed = false;
        if (bulk_mirror && any_mirror) {
            tty_redraw_all();
        }
        if (any_mirror || any_input || any_shell) {
            tty_cursor_show();
            fb_present();
        } else {
            mx_yield();
        }
    }
}

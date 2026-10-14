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
#include "mochi/driver.h"
#include "libk/str.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/framebuffer.h"
#include "mochi/input.h"
#include "mochi/terminal.h"
#include "mochi/va_layout.h"

using namespace mochi::va::tty;

// Text grid dimensions: cover the full 1024×768 ramfb with 8×16 cells.
// Keeping the terminal at 80×25 made boot logs hit the scroll path almost
// immediately, which can back-pressure the framebuffer command ring.
constexpr uint32_t kFg = 0xE6F1FF;
constexpr uint32_t kBg = 0x001020;

struct TtyState {
    uint32_t cols,rows,cell_w,cell_h,origin_x,origin_y,scale;
    mx_handle_t activation;
    mochi::framebuffer::Client fb;
    mochi::terminal::OutputSource mirror,shell_output;
    mochi::terminal::InputSource serial_input;
    mochi::terminal::OutputSink serial_output;
    mochi::input::EventSource input;
    mochi::terminal::InputSink shell_input;
    bool shell_attached,render_suppressed,cursor_visible,input_unlocked;
    uint64_t serial_in_dropped, serial_out_dropped;
    uint32_t cursor_row,cursor_col,esc_streak,scroll_pending;

    uint32_t cursor_x;
    uint32_t cursor_y;
    // V0: store character grid as plain ASCII for log mirror.
    char* cells;
};

static TtyState* g_tty;
static char* line(uint32_t row) { return g_tty->cells+row*g_tty->cols; }
// The tty -> shell input sink is the shared terminal input endpoint.
using ShellInputSink = mochi::terminal::InputSink;


constexpr uint32_t HID_KEY_RIGHT = 0x4Fu;
constexpr uint32_t HID_KEY_LEFT = 0x50u;
constexpr uint32_t HID_KEY_DOWN = 0x51u;
constexpr uint32_t HID_KEY_UP = 0x52u;
constexpr uint32_t HID_KEY_HOME = 0x4Au;
constexpr uint32_t HID_KEY_END = 0x4Du;
constexpr uint32_t HID_KEY_DELETE = 0x4Cu;
constexpr uint32_t HID_KEY_ESCAPE = 0x29u;

// Init explicitly activates the terminal after msh is ready. Logs are display
// content, never control messages. ESC bypass remains useful for recovery.
static constexpr uint32_t kEscBypassCount = 3;


static uint32_t slen(const char* s) {
    return static_cast<uint32_t>(libk::str::length(s));
}

static bool fb_send(uint32_t type, const void* data, uint32_t len) {
    // Framebuffer commands are the terminal's source of truth.  Dropping one
    // command leaves the visible text permanently truncated until a later full
    // redraw, which looks like a framebuffer stall.  Back-pressure tty.fb0
    // instead and let io.framebuffer drain at its own pace.
    return g_tty->fb.send_spinning(type, data, len, 0xFFFF'FFFFu);
}


static void fb_clear_screen() {
    if (g_tty->render_suppressed) return;
    const uint32_t color = kBg;
    (void)fb_send(FB_CLEAR, &color, sizeof(color));
}

static void fb_fill_cell(uint32_t row, uint32_t col) {
    if (g_tty->render_suppressed) return;
    FbFillRectCmd cmd{};
    cmd.x = g_tty->origin_x + col * g_tty->cell_w;
    cmd.y = g_tty->origin_y + row * g_tty->cell_h;
    cmd.width = g_tty->cell_w;
    cmd.height = g_tty->cell_h;
    cmd.color = kBg;
    (void)fb_send(FB_FILL_RECT, &cmd, sizeof(cmd));
}

static void fb_draw_text_run(uint32_t row, uint32_t col, const char* data, uint32_t len);

static void fb_draw_cell(uint32_t row, uint32_t col) {
    if (g_tty->render_suppressed) return;
    const char ch = line(row)[col];
    if (ch == ' ') {
        fb_fill_cell(row, col);
        return;
    }

    fb_draw_text_run(row, col, &ch, 1);
}

static void fb_draw_text_run(uint32_t row, uint32_t col, const char* data, uint32_t len) {
    if (!data || len == 0) return;
    if (g_tty->render_suppressed) return;

    uint32_t off = 0;
    while (off < len) {
        const uint32_t remaining = len - off;
        const uint32_t chunk = remaining > kFbDrawTextMaxChars
            ? static_cast<uint32_t>(kFbDrawTextMaxChars)
            : remaining;

        FbDrawTextCmd cmd{};
        cmd.x = g_tty->origin_x + (col + off) * g_tty->cell_w;
        cmd.y = g_tty->origin_y + row * g_tty->cell_h;
        cmd.fg = kFg;
        cmd.bg = kBg;
        cmd.len = static_cast<uint8_t>(chunk);
        cmd.scale_x = g_tty->scale;
        cmd.scale_y = g_tty->scale;
        for (uint32_t i = 0; i < chunk; ++i) {
            const char ch = data[off + i];
            cmd.text[i] = libk::str::is_ascii_printable(ch) ? ch : '?';
        }
        (void)fb_send(FB_DRAW_TEXT, &cmd, sizeof(cmd));
        off += chunk;
    }
}

static void tty_cursor_hide() {
    if (!g_tty->cursor_visible) return;
    if (g_tty->cursor_row < g_tty->rows && g_tty->cursor_col < g_tty->cols) {
        fb_draw_cell(g_tty->cursor_row, g_tty->cursor_col);
    }
    g_tty->cursor_visible = false;
}

static void tty_cursor_show() {
    if (g_tty->render_suppressed) return;
    const uint32_t row = g_tty->cursor_y < g_tty->rows ? g_tty->cursor_y : (g_tty->rows - 1u);
    const uint32_t col = g_tty->cursor_x < g_tty->cols ? g_tty->cursor_x : (g_tty->cols - 1u);
    FbFillRectCmd cmd{};
    cmd.x = g_tty->origin_x + col * g_tty->cell_w;
    cmd.y = g_tty->origin_y + row * g_tty->cell_h + (g_tty->cell_h - 2u);
    cmd.width = g_tty->cell_w;
    cmd.height = 2;
    cmd.color = kFg;
    (void)fb_send(FB_FILL_RECT, &cmd, sizeof(cmd));
    g_tty->cursor_row = row;
    g_tty->cursor_col = col;
    g_tty->cursor_visible = true;
}

static void tty_redraw_all() {
    tty_cursor_hide();
    fb_clear_screen();
    for (uint32_t r = 0; r < g_tty->rows; ++r) {
        fb_draw_text_run(r, 0, line(r), g_tty->cols);
    }
    (void)fb_send(FB_PRESENT, nullptr, 0);
}

// Fold all deferred scrolls from this drain batch into one repaint of the
// console region. The character grid is authoritative; the repaint reads only
// it, so interleaved draws and scrolls within a batch stay consistent.
static void flush_scroll() {
    if (!g_tty->scroll_pending) return;
    g_tty->scroll_pending = 0;
    tty_redraw_all();
}

static void fb_present() {
    if (g_tty->render_suppressed) return;
    (void)fb_send(FB_PRESENT, nullptr, 0);
}

static void tty_newline() {
    g_tty->cursor_x = 0;
    if (g_tty->cursor_y + 1 < g_tty->rows) {
        ++g_tty->cursor_y;
    } else {
        // Scroll: shift rows up.
        for (uint32_t r = 0; r + 1 < g_tty->rows; ++r) {
            (void)libk::copy_exact(libk::as_bytes(line(r),g_tty->cols),
                                    libk::as_bytes(line(r + 1),g_tty->cols));
        }
        libk::fill(libk::as_bytes(line(g_tty->rows - 1),g_tty->cols), ' ');
        // Defer the pixel work: flush_scroll() folds a whole batch of scrolled
        // lines into one repaint instead of moving the full pixel region
        // (~7 MB) for every line.
        ++g_tty->scroll_pending;
    }
}

static void tty_write_char(char ch) {
    tty_cursor_hide();
    if (ch == '\n') {
        tty_newline();
        return;
    }
    if (ch == '\r') {
        g_tty->cursor_x = 0;
        return;
    }
    if (ch == '\b') {
        if (g_tty->cursor_x > 0) {
            --g_tty->cursor_x;
            line(g_tty->cursor_y)[g_tty->cursor_x] = ' ';
            fb_draw_cell(g_tty->cursor_y, g_tty->cursor_x);
        }
        return;
    }
    if (ch == '\t') {
        const uint32_t next = (g_tty->cursor_x + 8u) & ~7u;
        while (g_tty->cursor_x < next && g_tty->cursor_x < g_tty->cols) {
            tty_write_char(' ');
        }
        return;
    }
    if (g_tty->cursor_x >= g_tty->cols) tty_newline();
    const uint32_t row = g_tty->cursor_y;
    const uint32_t col = g_tty->cursor_x++;
    line(row)[col] = libk::str::is_ascii_printable(ch) ? ch : '?';
    fb_draw_cell(row, col);
}

static void tty_write_printable_run(const char* s, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        if (g_tty->cursor_x >= g_tty->cols) tty_newline();

        const uint32_t row = g_tty->cursor_y;
        const uint32_t col = g_tty->cursor_x;
        const uint32_t room = g_tty->cols - col;
        uint32_t chunk = len - off;
        if (chunk > room) chunk = room;
        if (chunk > kFbDrawTextMaxChars) {
            chunk = static_cast<uint32_t>(kFbDrawTextMaxChars);
        }

        (void)libk::copy_n(libk::slice(libk::as_bytes(line(row),g_tty->cols), col, room),
                           libk::as_bytes(s + off, len - off), chunk);
        fb_draw_text_run(row, col, s + off, chunk);
        g_tty->cursor_x += chunk;
        off += chunk;
    }
}

static void tty_write(const char* s, uint32_t len) {
    tty_cursor_hide();
    uint32_t i = 0;
    while (i < len) {
        if (libk::str::is_ascii_printable(s[i])) {
            uint32_t run = 0;
            while ((i + run) < len && libk::str::is_ascii_printable(s[i + run])) ++run;
            tty_write_printable_run(s + i, run);
            i += run;
        } else {
            tty_write_char(s[i]);
            ++i;
        }
    }
}

static bool handle_mirror_slot(const RingSlot& slot) {
    const char* bytes = reinterpret_cast<const char*>(slot.payload);
    tty_write(bytes, slot.length);
    return true;
}

static bool send_shell_input_char(char ch) {
    if (!g_tty->shell_attached || !g_tty->shell_input.valid()) return false;
    return g_tty->shell_input.write_input_byte_spinning(ch, 4096);
}

static bool send_shell_input_bytes(const char* bytes, uint32_t len) {
    if (!bytes || len == 0) return false;
    for (uint32_t i = 0; i < len; ++i) {
        if (!send_shell_input_char(bytes[i])) return false;
    }
    return true;
}

static bool handle_input_event(const InputEvent& event) {
    if (!g_tty->input_unlocked) {
        // Pre-ready gate: count ESC streaks, drop everything else. INPUT_TEXT
        // events for the ESC code (0x1B) are not currently produced by the
        // existing key-press path, so the HID keycode is the only trigger.
        if (event.type == INPUT_KEY_DOWN || event.type == INPUT_KEY_REPEAT) {
            if (event.keycode == HID_KEY_ESCAPE) {
                ++g_tty->esc_streak;
                if (g_tty->esc_streak >= kEscBypassCount) {
                    g_tty->input_unlocked = true;
                    g_tty->esc_streak = 0;
                }
            } else {
                g_tty->esc_streak = 0;
            }
        }
        return false;
    }

    if (event.type == INPUT_KEY_DOWN || event.type == INPUT_KEY_REPEAT) {
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

// Raw serial console input (dev.serial): bytes from the host terminal feed
// the shell directly, the same sink the keyboard path writes into. A serial
// terminal sends exactly what msh's line editor expects, so nothing is
// translated; under shell backpressure bytes are dropped and counted.
static bool drain_serial_input(uint32_t max_slots) {
    if (!g_tty->serial_input.ring) return false;
    bool any = false;
    RingSlot slot{};
    uint32_t drained = 0;
    while (drained < max_slots && g_tty->serial_input.try_read(&slot)) {
        if (!g_tty->shell_input.try_write_bytes(slot.payload, slot.length)) {
            g_tty->serial_in_dropped += slot.length;
        }
        any = true;
        ++drained;
    }
    return any;
}

static bool drain_input(uint32_t max_slots) {
    if (!g_tty->input.ring) return false;
    bool any = false;
    InputEvent event{};
    uint32_t drained = 0;
    while (drained < max_slots && g_tty->input.try_read(&event)) {
        any = handle_input_event(event) || any;
        ++drained;
    }
    return any;
}

static bool drain_mirror(uint32_t max_slots) {
    if (!g_tty->mirror.ring) return false;
    bool any = false;
    RingSlot slot{};
    uint32_t drained = 0;
    while (drained < max_slots && g_tty->mirror.try_read(&slot)) {
        any = handle_mirror_slot(slot) || any;
        ++drained;
    }
    return any;
}

static bool drain_shell_output(uint32_t max_slots) {
    if (!g_tty->shell_output.ring) return false;
    // Activation is an explicit boot doorbell; preserve output until then.
    if (!g_tty->input_unlocked) return false;
    bool any = false;
    RingSlot slot{};
    uint32_t drained = 0;
    while (drained < max_slots && g_tty->shell_output.try_read(&slot)) {
        tty_write(reinterpret_cast<const char*>(slot.payload), slot.length);
        // Mirror the same bytes to the serial console bridge when attached.
        // The serial side drops under backpressure and never blocks rendering.
        if (g_tty->serial_output.ring &&
            !g_tty->serial_output.try_write_bytes(slot.payload, slot.length)) {
            g_tty->serial_out_dropped += slot.length;
        }
        any = true;
        ++drained;
    }
    return any;
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);

    (void)mochi::log_map_from_boot(boot, kVaLogRing, "tty.fb0");

    auto state = mochi::make_state(0x1b100000,131072);
    g_tty = state.alloc<TtyState>();
    if(!g_tty) mochi::boot::invalid_boot();
    g_tty->cols=mochi::driver::value(boot,"tty.cols",128); g_tty->rows=mochi::driver::value(boot,"tty.rows",48);
    g_tty->origin_x=mochi::driver::value(boot,"tty.x"); g_tty->origin_y=mochi::driver::value(boot,"tty.y");
    g_tty->scale=mochi::driver::value(boot,"tty.scale",1);
    if(!g_tty||!g_tty->cols||g_tty->cols>256||!g_tty->rows||g_tty->rows>256||!g_tty->scale||g_tty->scale>8) mochi::boot::invalid_boot();
    g_tty->cell_w=8*g_tty->scale; g_tty->cell_h=16*g_tty->scale;
    g_tty->activation=mochi::boot::require_handle(boot,"console.activate");
    g_tty->fb = {
        mochi::boot::map_ring(boot, {"fb.cmd.ring", kVaFbCmdRing, "fb.cmd.doorbell"}),
        mochi::boot::map_ring(boot, {"fb.reply.ring", kVaFbReplyRing, "fb.reply.doorbell"}),
    };
    g_tty->mirror = {mochi::boot::map_ring(boot,
        {"console.mirror.ring", kVaMirrorRing, "console.mirror.doorbell"})};
    g_tty->input = {mochi::boot::map_ring(boot,
        {"input.tty.ring", kVaInputRing, "input.tty.doorbell"})};
    g_tty->shell_input = {mochi::boot::map_ring(boot,
        {"term.input.ring", kVaShellInputRing, "term.input.doorbell"})};
    g_tty->shell_output = {mochi::boot::map_ring(boot,
        {"term.output.ring", kVaShellOutputRing, "term.output.doorbell"})};
    g_tty->serial_input = {mochi::boot::map_ring(boot,
        {"serial.in.ring", kVaSerialInRing, "serial.in.doorbell"})};
    g_tty->serial_output = {mochi::boot::map_ring(boot,
        {"serial.out.ring", kVaSerialOutRing, "serial.out.doorbell"})};
    g_tty->shell_attached = g_tty->shell_input.valid() && g_tty->shell_output.valid();
    if (g_tty->shell_attached) {
        mochi::log_raw_marker("tty.fb0: shell terminal attached\n");
    } else {
        mochi::log_raw_marker("tty.fb0: shell terminal not attached; local echo fallback\n");
    }

    FbInfo info{};
    if(!g_tty->fb.request_info()) mochi::boot::invalid_boot();
    const auto geometry_deadline=mx_clock_ns()+3000000000ull;
    while(!g_tty->fb.try_read_info(&info)) {
        if(mx_clock_ns()>=geometry_deadline) mochi::boot::invalid_boot(); mx_yield();
    }
    if(!mochi::boot::find(boot,"tty.cols")) g_tty->cols=info.width/g_tty->cell_w;
    if(!mochi::boot::find(boot,"tty.rows")) g_tty->rows=info.height/g_tty->cell_h;
    if(g_tty->cols>256||g_tty->rows>256) mochi::boot::invalid_boot();
    g_tty->cells=state.alloc<char>(g_tty->rows*g_tty->cols);
    if(!g_tty->cells) mochi::boot::invalid_boot();
    if(g_tty->origin_x>info.width||g_tty->origin_y>info.height||
       g_tty->cols*g_tty->cell_w>info.width-g_tty->origin_x||
       g_tty->rows*g_tty->cell_h>info.height-g_tty->origin_y) mochi::boot::invalid_boot();
    libk::fill(libk::as_bytes(g_tty->cells,g_tty->rows*g_tty->cols), ' ');

    fb_clear_screen();
    mochi::log_raw_marker("tty.fb0: ready\n");
    const char* ready = "tty.fb0: Midix framebuffer terminal ready\n";
    tty_write(ready, slen(ready));
    tty_cursor_show();
    fb_present();

    mochi::SpscEndpoint readers[5]{};
    readers[0].rx_ring=g_tty->input.ring; readers[0].order=g_tty->input.order;
    readers[1].rx_ring=g_tty->mirror.ring; readers[1].order=g_tty->mirror.order;
    readers[2].rx_ring=g_tty->shell_output.ring; readers[2].order=g_tty->shell_output.order;
    readers[3].rx_ring=g_tty->fb.reply.ring; readers[3].order=g_tty->fb.reply.order;
    readers[4].rx_ring=g_tty->serial_input.ring; readers[4].order=g_tty->serial_input.order;
    mochi::boot::ready(boot);
    while (true) {
        if(!g_tty->input_unlocked&&mx_doorbell_read(g_tty->activation)) g_tty->input_unlocked=true;
        const bool any_input = g_tty->input_unlocked&&drain_input(64);
        const bool any_serial = g_tty->input_unlocked&&drain_serial_input(64);
        const bool any_shell = drain_shell_output(64);
        const uint32_t mirror_depth = g_tty->mirror.depth();
        const bool bulk_mirror = mirror_depth > 8;
        g_tty->render_suppressed = bulk_mirror;
        const uint32_t mirror_budget = bulk_mirror ? mirror_depth : 4;
        const bool any_mirror = drain_mirror(mirror_budget);
        g_tty->render_suppressed = false;
        if (bulk_mirror && any_mirror) {
            g_tty->scroll_pending = 0; // covered by the whole-grid repaint below
            tty_redraw_all();
        }
        flush_scroll();
        if (any_mirror || any_input || any_serial || any_shell) {
            tty_cursor_show();
            fb_present();
        } else {
            mochi::driver::idle(readers,5,g_tty->input.doorbell,mx_clock_ns()+10000000);
        }
    }
}

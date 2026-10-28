// tty.fb0 owns shell-input multiplexing and terminal rendering. USB/CDC
// stays in dev.serial/usb.fn.acm; render.h and transport.h have no boot policy.
#include "render.h"
#include "transport.h"
#include "mochi/driver.h"
#include "mochi/input.h"
#include "mochi/log.h"
#include "mochi/boot_log.h"
#include "mochi/fido2_prompt.h"
using namespace mochi::va::tty;
namespace {
struct TtyState {
    mochi::tty::Render render;
    mochi::tty::InputBuffer pending_input;
    mochi::tty::SerialMirror serial;
    mochi::terminal::OutputSource mirror, shell_output;
    mochi::Ring serial_input;
    mochi::Ring fido_prompt;
    mochi::fido2::PromptView fido_view;
    mochi::input::EventSource input;
    mochi::terminal::InputSink shell_input;
    mx_handle_t activation;
    mx_u32_t esc_streak, next_input;
    // Mirrors the CDC bridge's configured state. On a USB-console board the
    // serial host *is* the terminal, so an attached host opens the same gate a
    // local keyboard opens: requiring three ESC keycodes from a device that may
    // have no keyboard at all left the console permanently deaf.
    bool serial_attached;
    bool shell_attached, input_unlocked;
};
TtyState* g_tty;

// US layout/text policy belongs to ui.input. This is only the terminal's
// escape-byte encoding for navigation keys; serial bytes already carry it.
struct KeyRun {
    mx_u32_t key;
    const char* bytes;
    mx_u8_t count;
};
constexpr KeyRun kNavigation[] = {
    {0x52, "\x1b[A", 3}, {0x51, "\x1b[B", 3}, {0x4f, "\x1b[C", 3},  {0x50, "\x1b[D", 3},
    {0x4a, "\x1b[H", 3}, {0x4d, "\x1b[F", 3}, {0x4c, "\x1b[3~", 4},
};
// One predicate for every gate below, so "console is live" cannot drift apart
// between the input, output and drain paths.
static inline bool console_live() {
    return g_tty->input_unlocked || g_tty->serial_attached;
}

bool keyboard(const InputEvent& event) {
    if (!console_live()) {
        if (event.type == INPUT_KEY_DOWN || event.type == INPUT_KEY_REPEAT) {
            g_tty->esc_streak = event.keycode == 0x29 ? g_tty->esc_streak + 1 : 0;
            if (g_tty->esc_streak == 3) {
                g_tty->input_unlocked = true;
            }
        }
        return false;
    }
    if (event.type == INPUT_KEY_DOWN || event.type == INPUT_KEY_REPEAT) {
        for (const auto& key : kNavigation) {
            if (event.keycode == key.key) {
                return g_tty->pending_input.hold(libk::as_bytes(key.bytes, key.count));
            }
        }
    }
    if (event.type != INPUT_TEXT) {
        return false;
    }
    const auto cp = event.unicode;
    if (!(cp == '\n' || cp == '\r' || cp == '\b' || cp == '\t' || cp == 1 || cp == 2 || cp == 3 ||
          cp == 4 || cp == 5 || cp == 6 || cp == 11 || cp == 12 || cp == 14 || cp == 16 ||
          cp == 21 || (cp >= 32 && cp <= 126))) {
        return false;
    }
    const char byte = cp == '\r' ? '\n' : static_cast<char>(cp);
    if (!g_tty->shell_attached) {
        g_tty->render.write(libk::as_bytes(&byte, 1));
        return true;
    }
    return g_tty->pending_input.hold(libk::as_bytes(&byte, 1));
}
bool service_input() {
    if (g_tty->pending_input.retained()) {
        return g_tty->pending_input.flush(g_tty->shell_input);
    }
    // Both sources feed one retained buffer and one SPSC producer. Alternate
    // the first reader so serial traffic cannot starve a physical keyboard.
    for (mx_u32_t n = 0; n < 2; ++n) {
        const auto index = (g_tty->next_input + n) & 1;
        if (!index) {
            InputEvent event{};
            if (!g_tty->input.try_read(&event)) {
                continue;
            }
            (void)keyboard(event);
        } else {
            MxRingSlot slot{};
            if (!g_tty->serial_input.try_pop(&slot)) {
                continue;
            }
            // Session control is lifecycle, not user input: it carries the
            // generation every byte must match. Consuming it regardless of the
            // gate is what lets an attached host become usable at all, because
            // the FLOW_ON record is produced by the same bridge that the gate
            // would otherwise silence.
            if (slot.type == MX_TERM_CONTROL) {
                (void)mochi::tty::accept_serial(slot, g_tty->serial, g_tty->pending_input);
                g_tty->serial_attached = g_tty->serial.generation != 0;
                g_tty->next_input = index ^ 1;
                return true;
            }
            if (!console_live()) {
                g_tty->next_input = index ^ 1;
                return true; // bytes for a closed console are consumed, not queued
            }
            (void)mochi::tty::accept_serial(slot, g_tty->serial, g_tty->pending_input);
        }
        g_tty->next_input = index ^ 1;
        return true; // a consumed non-text/control record is still progress
    }
    return false;
}
bool service_output() {
    bool progress = g_tty->serial.flush();
    // Shell bytes stay queued while the serial mirror owns a blocked suffix.
    // Offline mirrors consume quickly; a slow live host applies backpressure.
    for (mx_u32_t i = 0; i < 64 && console_live() && g_tty->serial.pending.empty(); ++i) {
        MxRingSlot slot{};
        if (!g_tty->shell_output.try_read(&slot)) {
            break;
        }
        g_tty->render.write(libk::as_bytes(slot.payload, slot.length));
        (void)g_tty->serial.hold(slot);
        (void)g_tty->serial.flush();
        progress = true;
    }
    for (mx_u32_t i = 0; i < 4; ++i) {
        MxRingSlot slot{};
        if (!g_tty->mirror.try_read(&slot)) {
            break;
        }
        g_tty->render.write(libk::as_bytes(slot.payload, slot.length));
        progress = true;
    }
    MxRingSlot notice{};
    bool show_prompt = g_tty->fido_view.expire(mx_clock_ns());
    while (g_tty->fido_prompt.try_pop(&notice)) {
        show_prompt |= g_tty->fido_view.accept(notice, mx_clock_ns());
        progress = true;
    }
    if (show_prompt) {
        const auto& view = g_tty->fido_view;
        const auto write = [](const char* text) {
            g_tty->render.write(libk::as_bytes(text, libk::str::length(text)));
        };
        if (view.state == MX_FIDO2_WAITING) {
            write("\n[Security key]\n");
            if (view.operation == MX_FIDO2_SELECT) {
                write("Select this key for the browser.");
            } else {
                write(view.operation == 1 ? "Register with: " : "Sign in to: ");
                write(view.rp);
            }
            write("\nPress VOLUME UP to approve within 30 seconds.\n");
        } else {
            write(view.state == MX_FIDO2_SELECTED
                      ? "\nSecurity key: selected. Continue in the browser.\n"
                  : view.state == MX_FIDO2_APPROVED  ? "\nSecurity key: approved.\n"
                  : view.state == MX_FIDO2_CANCELLED ? "\nSecurity key: cancelled.\n"
                  : view.state == MX_FIDO2_EXPIRED   ? "\nSecurity key: request expired.\n"
                                                     : "\nSecurity key: request rejected.\n");
        }
        progress = true;
    }
    // INFO is consumed during startup. Discard unsolicited framebuffer replies
    // rather than leaving an unread reply ring permanently runnable.
    MxRingSlot reply{};
    for (mx_u32_t i = 0; i < 16 && g_tty->render.fb.reply.try_pop(&reply); ++i) {
        progress = true;
    }
    return progress;
}
} // namespace
extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);

    (void)mochi::log_map_from_boot(boot, kVaLogRing, "tty.fb0");

    auto state = mochi::make_state(0x1b100000, 131072);
    g_tty = state.alloc<TtyState>();
    if (!g_tty) {
        mochi::boot::invalid_boot();
    }
    g_tty->render.cols = mochi::driver::value(boot, "tty.cols", 128);
    g_tty->render.rows = mochi::driver::value(boot, "tty.rows", 48);
    g_tty->render.origin_x = mochi::driver::value(boot, "tty.x");
    g_tty->render.origin_y = mochi::driver::value(boot, "tty.y");
    g_tty->render.scale = mochi::driver::value(boot, "tty.scale", 1);
    if (!g_tty->render.cols || g_tty->render.cols > 256 || !g_tty->render.rows ||
        g_tty->render.rows > 256 || !g_tty->render.scale || g_tty->render.scale > 8) {
        mochi::boot::invalid_boot();
    }
    g_tty->render.cell_w = 8 * g_tty->render.scale;
    g_tty->render.cell_h = 16 * g_tty->render.scale;
    g_tty->activation = mochi::boot::require_handle(boot, "console.activate");
    g_tty->render.fb = {
        mochi::boot::map_ring(boot, {"fb.cmd.ring", kVaFbCmdRing, "fb.cmd.doorbell"}),
        mochi::boot::map_ring(boot, {"fb.reply.ring", kVaFbReplyRing, "fb.reply.doorbell"}),
    };
    g_tty->mirror = {mochi::boot::map_ring(
        boot, {"console.mirror.ring", kVaMirrorRing, "console.mirror.doorbell"})};
    g_tty->input = {
        mochi::boot::map_ring(boot, {"input.tty.ring", kVaInputRing, "input.tty.doorbell"})};
    g_tty->shell_input = {
        mochi::boot::map_ring(boot, {"term.input.ring", kVaShellInputRing, "term.input.doorbell"})};
    g_tty->shell_output = {mochi::boot::map_ring(
        boot, {"term.output.ring", kVaShellOutputRing, "term.output.doorbell"})};
    g_tty->serial_input =
        mochi::boot::map_ring(boot, {"serial.in.ring", kVaSerialInRing, "serial.in.doorbell"});
    g_tty->fido_prompt =
        mochi::boot::map_ring(boot, {"fido2.prompt.ring", kVaFido2Prompt, "fido2.prompt.doorbell"});
    g_tty->serial.output = {
        mochi::boot::map_ring(boot, {"serial.out.ring", kVaSerialOutRing, "serial.out.doorbell"})};
    g_tty->shell_attached = g_tty->shell_input.valid() && g_tty->shell_output.valid();
    if (g_tty->shell_attached) {
        mochi::log_raw_marker("tty.fb0: shell terminal attached\n");
    } else {
        mochi::log_raw_marker("tty.fb0: shell terminal not attached; local echo fallback\n");
    }

    FbInfo info{};
    if (!g_tty->render.fb.request_info()) {
        mochi::boot::invalid_boot();
    }
    const auto geometry_deadline = mx_clock_ns() + 3000000000ull;
    while (!g_tty->render.fb.try_read_info(&info)) {
        if (mx_clock_ns() >= geometry_deadline) {
            mochi::boot::invalid_boot();
        }
        mochi::SpscEndpoint reply{};
        reply.rx_ring = g_tty->render.fb.reply.ring;
        reply.order = g_tty->render.fb.reply.order;
        mochi::driver::idle(&reply, 1, g_tty->render.fb.reply.doorbell, geometry_deadline);
    }
    auto& render = g_tty->render;
    const auto bar_height = mochi::driver::value(boot, "tty.status.height", 64);
    if (mochi::driver::value(boot, "tty.status", 1) && bar_height < info.height &&
        render.status.configure(info.width, bar_height,
                                mochi::driver::value(boot, "tty.status.scale", 1),
                                mochi::driver::value(boot, "tty.status.margin", 16),
                                mochi::driver::value(boot, "tty.status.notch", 0))) {
        if (!mochi::boot::find(boot, "tty.y")) {
            render.origin_y = bar_height + 16;
        }
        // Explicit terminal geometry must leave the status panels untouched.
        if (render.origin_y < bar_height) {
            render.status.enabled = false;
        }
    }
    if (render.origin_x > info.width || render.origin_y > info.height) {
        mochi::boot::invalid_boot();
    }
    if (!mochi::boot::find(boot, "tty.cols")) {
        g_tty->render.cols = (info.width - g_tty->render.origin_x) / g_tty->render.cell_w;
    }
    if (!mochi::boot::find(boot, "tty.rows")) {
        g_tty->render.rows = (info.height - g_tty->render.origin_y) / g_tty->render.cell_h;
    }
    if (!g_tty->render.cols || !g_tty->render.rows || g_tty->render.cols > 256 ||
        g_tty->render.rows > 256) {
        mochi::boot::invalid_boot();
    }
    g_tty->render.cells = state.alloc<char>(g_tty->render.rows * g_tty->render.cols);
    if (!g_tty->render.cells) {
        mochi::boot::invalid_boot();
    }
    if (g_tty->render.origin_x > info.width || g_tty->render.origin_y > info.height ||
        g_tty->render.cols * g_tty->render.cell_w > info.width - g_tty->render.origin_x ||
        g_tty->render.rows * g_tty->render.cell_h > info.height - g_tty->render.origin_y) {
        mochi::boot::invalid_boot();
    }

    g_tty->render.init();
    mochi::log_raw_marker("tty.fb0: ready\n");
    mochi::SpscEndpoint readers[6]{};
    readers[0].rx_ring = g_tty->input.ring;
    readers[0].order = g_tty->input.order;
    readers[1].rx_ring = g_tty->mirror.ring;
    readers[1].order = g_tty->mirror.order;
    readers[2].rx_ring = g_tty->shell_output.ring;
    readers[2].order = g_tty->shell_output.order;
    readers[3].rx_ring = g_tty->render.fb.reply.ring;
    readers[3].order = g_tty->render.fb.reply.order;
    readers[4].rx_ring = g_tty->serial_input.ring;
    readers[4].order = g_tty->serial_input.order;
    readers[5].rx_ring = g_tty->fido_prompt.ring;
    readers[5].order = g_tty->fido_prompt.order;
    mochi::boot::ready(boot);
    for (;;) {
        const auto expected = mx_doorbell_read(g_tty->input.doorbell);
        if (!g_tty->input_unlocked && mx_doorbell_read(g_tty->activation)) {
            g_tty->input_unlocked = true;
        }
        if (render.status.sample(mx_clock_ns())) {
            render.present_pending = true;
        }
        const bool exhausted = mochi::driver::run_batch([] {
            const bool input = service_input();
            const bool output = service_output();
            return g_tty->render.flush() || input || output;
        });
        if (exhausted) {
            mx_yield();
            continue;
        }
        const bool input_blocked = g_tty->pending_input.retained();
        const bool output_blocked = !g_tty->serial.pending.empty();
        const bool retry = input_blocked || output_blocked || g_tty->render.pending();
        auto deadline = retry             ? mx_clock_ns() + mochi::driver::kBackpressureRetryNs
                        : !console_live() ? mx_clock_ns() + 10000000ull
                                          : 0;
        if (g_tty->fido_view.expires && (!deadline || g_tty->fido_view.expires < deadline)) {
            deadline = g_tty->fido_view.expires;
        }
        if (render.status.enabled && !render.status.pending() &&
            (!deadline || render.status.next_sample < deadline)) {
            deadline = render.status.next_sample;
        }
        const mx_u64_t readable = (!input_blocked ? 1u : 0u) | 2u | 8u |
                                  (console_live() && !output_blocked ? 4u : 0u) |
                                  (console_live() && !input_blocked ? 16u : 0u) | 32u;
        mochi::driver::idle_on_counter(readers, 6, g_tty->input.doorbell, expected, deadline,
                                       readable);
    }
}

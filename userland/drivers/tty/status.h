#pragma once
#include "mochi/framebuffer.h"
#include "midix/abi/diagnostics.h"
#include "libk/str.h"

namespace mochi::tty {
// Publishes one bounded command at a time on the terminal's existing SPSC ring.
// A frame stays immutable until admitted, including under framebuffer backpressure.
struct StatusBar {
    static constexpr mx_u32_t kBackground = 0x0b1827, kMuted = 0x7890a6;
    static constexpr mx_u32_t kText = 0xe6f1ff, kAccent = 0x74d9cf, kLine = 0x23384c;
    struct Command {
        mx_u32_t type;
        FbFillRectCmd rect;
        FbDrawTextCmd text;
    } commands[24];
    mx_u32_t count, sent, width, height, scale, margin, notch;
    mx_u64_t next_sample;
    MxDiagHealth previous;
    bool enabled, baseline, drawn;
    mx_u32_t shown_cpu, shown_ram, shown_battery;

    bool configure(mx_u32_t w, mx_u32_t h, mx_u32_t s, mx_u32_t inset,
                   mx_u32_t notch_width = 0) {
        // Two 13-cell panels plus a protected central cutout, all in pixels.
        const mx_u64_t need = mx_u64_t{inset} * 2 + mx_u64_t{notch_width} + 208ull * s;
        enabled = s && s <= 8 && h >= 48 * s && need <= w;
        if (enabled) { width = w; height = h; scale = s; margin = inset; notch = notch_width; }
        return enabled;
    }
    bool pending() const { return sent < count; }
    void rect(mx_u32_t x, mx_u32_t y, mx_u32_t w, mx_u32_t h, mx_u32_t color) {
        auto& command = commands[count++];
        command = {}; command.type = FB_FILL_RECT; command.rect = {x, y, w, h, color};
    }
    void text(mx_u32_t x, mx_u32_t y, const char* value, mx_u32_t color) {
        auto& command = commands[count++];
        command = {}; command.type = FB_DRAW_TEXT;
        auto& t = command.text;
        t.x = x; t.y = y; t.fg = color; t.bg = kBackground;
        t.scale_x = t.scale_y = scale;
        t.len = libk::str::length(value);
        libk::copy_n(libk::as_bytes(t.text), libk::as_bytes(value, t.len), t.len);
    }
    void metric(mx_u32_t y, const char* label, mx_u32_t percent, bool valid) {
        char value[] = "CPU  --%";
        for (unsigned i = 0; i < 3; ++i) value[i] = label[i];
        if (valid) {
            value[4] = percent == 100 ? '1' : ' ';
            value[5] = percent >= 10 ? '0' + percent / 10 % 10 : ' ';
            value[6] = '0' + percent % 10;
        }
        text(margin, y, value, kText);
        const auto x = margin + 72 * scale;
        rect(x, y + 6 * scale, 32 * scale, 4 * scale, kLine);
        if (valid && percent) rect(x, y + 6 * scale, 32 * scale * percent / 100,
                                   4 * scale, kAccent);
    }
    void frame(const MxDiagHealth* health) {
        count = sent = 0;
        const bool valid = health && health->version == MX_DIAG_VERSION;
        bool cpu_valid = false;
        mx_u32_t cpu = 0, ram = 0;
        if (valid && baseline && health->online_mask && health->online_mask == previous.online_mask &&
            health->sampled_ns > previous.sampled_ns && health->busy_ns >= previous.busy_ns) {
            const auto elapsed = health->sampled_ns - previous.sampled_ns;
            const auto cores = static_cast<mx_u32_t>(__builtin_popcountll(health->online_mask));
            const auto delta = health->busy_ns - previous.busy_ns;
            // Divide first to keep long-lived cumulative counters out of the multiply.
            const auto per_core = delta / cores;
            cpu = per_core >= elapsed ? 100 : static_cast<mx_u32_t>(per_core * 100 / elapsed);
            cpu_valid = true;
        }
        const bool memory_valid = valid && health->total_bytes && health->used_bytes <= health->total_bytes;
        if (memory_valid) ram = health->used_bytes * 100 / health->total_bytes;
        baseline = valid;
        if (valid) previous = *health;
        const bool battery = valid && (health->flags & MX_HEALTH_BATTERY_VOLTAGE) &&
                             health->battery_mv >= 2500 && health->battery_mv <= 5000;
        const auto cpu_key = cpu_valid ? cpu : 101;
        const auto ram_key = memory_valid ? ram : 101;
        const auto battery_key = battery ? (health->battery_mv + 5) / 10 : 0;
        if (drawn && shown_cpu == cpu_key && shown_ram == ram_key && shown_battery == battery_key)
            return;
        drawn = true; shown_cpu = cpu_key; shown_ram = ram_key; shown_battery = battery_key;
        rect(0, 0, width, height, kBackground);
        if (notch) rect((width - notch) / 2, 0, notch, height - 2, 0x000000);
        rect(0, height - 2, width, 2, kLine);
        const auto y = (height - 40 * scale) / 2;
        metric(y, "CPU", cpu, cpu_valid);
        metric(y + 24 * scale, "RAM", ram, memory_valid);
        const auto x = width - margin - 104 * scale;
        const auto by = (height - 16 * scale) / 2;
        // An outline represents a battery, without implying a fabricated charge level.
        rect(x, by + 2 * scale, 18 * scale, 12 * scale, kMuted);
        rect(x + 2 * scale, by + 4 * scale, 14 * scale, 8 * scale, kBackground);
        rect(x + 18 * scale, by + 6 * scale, 2 * scale, 4 * scale, kMuted);
        char volts[] = "BAT --.--V";
        if (battery) {
            const auto mv = (health->battery_mv + 5) / 10;
            // Compact label: BAT 4.02V (the leading space keeps a fixed 10-cell run).
            volts[4] = ' '; volts[5] = '0' + mv / 100;
            volts[6] = '.'; volts[7] = '0' + mv / 10 % 10; volts[8] = '0' + mv % 10;
        }
        text(x + 24 * scale, by, volts, battery && battery_key < 350 ? 0xffb878 : kText);
    }
    bool sample(mx_u64_t now) {
        if (!enabled || pending() || now < next_sample) return false;
        MxDiagHealth health{};
        const auto rc = sys_debug_read(MX_DIAG_HEALTH, 0, &health, sizeof(health));
        frame(rc == sizeof(health) ? &health : nullptr);
        next_sample = now + 1000000000ull;
        return pending();
    }
    bool flush(const mochi::framebuffer::Client& fb) {
        if (!pending()) return false;
        const auto& c = commands[sent];
        const bool ok = c.type == FB_FILL_RECT ? fb.try_send(c.type, &c.rect, sizeof(c.rect))
                                             : fb.try_send(c.type, &c.text, sizeof(c.text));
        if (ok) ++sent;
        return ok;
    }
};
} // namespace mochi::tty

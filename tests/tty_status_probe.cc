#include "../userland/drivers/tty/render.h"
#include "../userland/lib/include/mochi/framebuffer_raster.h"
#include "../boards/huawei-anne/battery.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static MxDiagHealth health{};
static bool available = true;
static unsigned samples;
static mx_i64_t read_health(mx_u64_t request, mx_u64_t index, void* out, mx_u64_t size) {
    assert(request == MX_DIAG_HEALTH && !index && size == sizeof(health));
    ++samples;
    if (!available) return MX_ERR_NOT_IMPLEMENTED;
    std::memcpy(out, &health, size);
    return size;
}
int main(int argc, char** argv) {
    // Verify PMUSSI byte stride, device identification, trims, invalid samples,
    // and that querying voltage never mutates any byte in the register bank.
    unsigned char pmic[4096];
    std::memset(pmic, 0xa5, sizeof(pmic));
    auto set = [&](unsigned reg, unsigned char byte) { pmic[reg * 4] = byte; };
    set(4, 1); set(0x182, 0); set(0x183, 0);
    set(0x1f4, 18000 & 255); set(0x1f5, 18000 >> 8);
    const std::vector<unsigned char> saved(pmic, pmic + sizeof(pmic));
    unsigned mv;
    assert(anne::battery::voltage_mv({pmic, sizeof(pmic)}, mv) && mv == 3917);
    assert(!std::memcmp(pmic, saved.data(), sizeof(pmic)));
    assert(!anne::battery::voltage_mv({pmic, 1024}, mv) && mv == 0);
    set(4, 2); assert(!anne::battery::voltage_mv({pmic, sizeof(pmic)}, mv));
    set(4, 1); set(0x1f5, 0xff);
    assert(!anne::battery::voltage_mv({pmic, sizeof(pmic)}, mv));
    set(0x1f4, 0); set(0x1f5, 0);
    assert(!anne::battery::voltage_mv({pmic, sizeof(pmic)}, mv));

    host_on_debug_read = read_health;
    mochi::tty::StatusBar bar{};
    assert(!bar.configure(200, 64, 1, 16, 0));
    assert(!bar.configure(1080, 128, 2, 64, 900));
    assert(bar.configure(1080, 128, 2, 64, 448));
    struct { MxRingPage page{}; MxRingSlot slots[4]{}; } ring;
    mochi::framebuffer::Client fb{{&ring.page, 2, 1}, {}};
    std::vector<uint32_t> pixels(1080 * 400, 0x001020);
    mochi::framebuffer::Raster raster{pixels.data(), 1080, 400};
    auto drain = [&] {
        while (bar.pending()) {
            assert(bar.flush(fb));
            MxRingSlot slot{}; assert(fb.cmd.try_pop(&slot));
            if (slot.type == FB_FILL_RECT) {
                FbFillRectCmd c; assert(mochi::message_get(slot, &c, FB_FILL_RECT));
                assert(c.x + c.width <= 1080 && c.y + c.height <= 128);
                raster.fill_rect(c.x, c.y, c.width, c.height, c.color);
            } else {
                FbDrawTextCmd c; assert(mochi::message_get(slot, &c, FB_DRAW_TEXT));
                assert(c.y + 16 * c.scale_y <= 128);
                assert(c.x + c.len * 8 * c.scale_x <= 316 || c.x >= 764);
                raster.draw_text(c);
            }
        }
    };
    health = {MX_DIAG_VERSION, MX_HEALTH_BATTERY_VOLTAGE, 1000000000, 0xff, 100000000,
              4ull << 30, 1ull << 30, 4020, 0};
    assert(bar.sample(0)); drain();
    assert(bar.shown_cpu == 101 && bar.shown_ram == 25 && bar.shown_battery == 402);
    const auto first = samples;
    assert(!bar.sample(999999999) && samples == first);
    health.sampled_ns += 1000000000; health.busy_ns += 4000000000;
    assert(bar.sample(1000000000));
    assert(bar.shown_cpu == 50); // half of eight CPUs, rather than 400%
    // Fill a four-slot ring, then confirm publication resumes at the same command.
    for (unsigned i = 0; i < 4; ++i) assert(fb.try_send(FB_PRESENT, nullptr, 0));
    const auto sent = bar.sent;
    assert(!bar.flush(fb) && bar.sent == sent);
    assert(!bar.sample(2000000000)); // never replace a partly admitted frame
    MxRingSlot slot{}; while (fb.cmd.try_pop(&slot)) {}
    drain();
    for (unsigned y = 0; y < 126; ++y) for (unsigned x = 316; x < 764; ++x)
        assert(pixels[y * 1080 + x] == 0xff000000); // protected notch region
    for (unsigned y = 128; y < 400; ++y) for (unsigned x = 0; x < 1080; ++x)
        assert(pixels[y * 1080 + x] == 0x001020); // terminal untouched

    if (argc == 2) {
        // Optional preview uses the actual font and rasterizer, not a mockup.
        FILE* out = std::fopen(argv[1], "wb"); assert(out);
        std::fprintf(out, "P6\n1080 400\n255\n");
        for (auto pixel : pixels) {
            const unsigned char rgb[] = {static_cast<unsigned char>(pixel >> 16),
                static_cast<unsigned char>(pixel >> 8), static_cast<unsigned char>(pixel)};
            std::fwrite(rgb, 1, 3, out);
        }
        std::fclose(out);
    }
    // The real terminal clear and pixel-copy scrolling preserve the status bar.
    mochi::tty::Render render{};
    std::vector<char> cells(59 * 3);
    render.fb = fb; render.cells = cells.data(); render.cols = 59; render.rows = 3;
    render.cell_w = 16; render.cell_h = 32; render.scale = 2;
    render.origin_x = 64; render.origin_y = 224;
    assert(render.status.configure(1080, 128, 2, 64, 448));
    render.init(); assert(render.status.sample(0));
    auto flush_terminal = [&] {
        for (unsigned i = 0; render.pending(); ++i) {
            assert(i < 1000); render.flush(3);
            while (fb.cmd.try_pop(&slot)) {
                if (slot.type == FB_CLEAR) {
                    unsigned color; assert(mochi::message_get(slot, &color, FB_CLEAR));
                    raster.clear(color);
                } else if (slot.type == FB_FILL_RECT) {
                    FbFillRectCmd c; assert(mochi::message_get(slot, &c, FB_FILL_RECT));
                    raster.fill_rect(c.x, c.y, c.width, c.height, c.color);
                } else if (slot.type == FB_DRAW_TEXT) {
                    FbDrawTextCmd c; assert(mochi::message_get(slot, &c, FB_DRAW_TEXT));
                    raster.draw_text(c);
                } else if (slot.type == FB_COPY_RECT) {
                    FbCopyRectCmd c; assert(mochi::message_get(slot, &c, FB_COPY_RECT));
                    raster.copy_rect(c);
                } else assert(slot.type == FB_PRESENT);
            }
        }
    };
    flush_terminal();
    const std::vector<uint32_t> top(pixels.begin(), pixels.begin() + 1080 * 128);
    for (unsigned i = 0; i < 50; ++i) {
        const char line[] = "console scroll test\n";
        render.write(libk::as_bytes(line, sizeof(line) - 1)); flush_terminal();
        assert(std::equal(top.begin(), top.end(), pixels.begin()));
    }
    // Unchanged display values generate no framebuffer traffic.
    health.sampled_ns += 1000000000; health.busy_ns += 4000000000;
    assert(!bar.sample(2000000000));
    health.online_mask = 1; health.sampled_ns += 1000000000;
    assert(bar.sample(3000000000)); assert(bar.shown_cpu == 101); drain();
    available = false;
    assert(bar.sample(4000000000));
    assert(bar.shown_ram == 101 && !bar.shown_battery); drain();
    available = true; health.sampled_ns += 1000000000;
    assert(bar.sample(5000000000)); assert(bar.shown_cpu == 101); drain();
    health.sampled_ns += 1000000000; health.busy_ns += 2000000000;
    health.used_bytes = health.total_bytes;
    assert(bar.sample(6000000000));
    assert(bar.shown_cpu == 100 && bar.shown_ram == 100); drain();
}

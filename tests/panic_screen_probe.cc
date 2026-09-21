// tests/panic_screen_probe.cc — host test for the kernel panic screen.
//
// Compiles kernel/panic_screen.cc against a synthetic XRGB8888 framebuffer (see
// tests/test_panic_screen.py) and asserts the screen renders the logo, headline
// and detail line, scales the logo to fit a range of display sizes, keeps every
// write inside the framebuffer, and renders deterministically.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "anko/panic_screen.h"

namespace {

constexpr uint32_t kMargin = 8;
constexpr uint32_t kHeadScale = 2;
constexpr uint32_t kCellH = 16;

struct Frame {
    uint32_t width;
    uint32_t height;
    std::vector<uint32_t> px;
    Frame(uint32_t w, uint32_t h)
        : width(w), height(h), px(static_cast<size_t>(w) * h, 0xDEADBEEFu) {}
};

struct Box {
    uint32_t count = 0;
    uint32_t x_min = 0xFFFFFFFFu, x_max = 0, y_min = 0xFFFFFFFFu, y_max = 0;
    uint32_t width() const { return count ? x_max - x_min + 1 : 0; }
    uint32_t height() const { return count ? y_max - y_min + 1 : 0; }
};

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

struct Layout {
    uint32_t art_top;
    uint32_t art_bottom;
    uint32_t detail_top;
};

Layout layout_of(uint32_t w, uint32_t h) {
    (void)w;
    const uint32_t detail_h = kCellH;
    const uint32_t art_top = kMargin + kCellH * kHeadScale + kMargin;
    const uint32_t detail_top = h > (detail_h + 2u * kMargin) ? h - (detail_h + 2u * kMargin) : art_top;
    return {art_top, detail_top, detail_top};
}

void run_size(uint32_t w, uint32_t h, const char* label) {
    const Layout L = layout_of(w, h);
    Frame f(w, h);
    anko::panic_screen::bind(reinterpret_cast<volatile uint32_t*>(f.px.data()), w, h);
    check(anko::panic_screen::bound(), "bound() after a valid bind");
    anko::panic_screen::show("KERNEL PANIC", "panic screen self-test");

    const uint32_t bg = f.px[0];
    Box headline, art, detail, all;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (f.px[static_cast<size_t>(y) * w + x] == bg) continue;
            Box* band = (y < L.art_top) ? &headline : (y < L.detail_top ? &art : &detail);
            for (Box* b : {band, &all}) {
                ++b->count;
                if (x < b->x_min) b->x_min = x;
                if (x > b->x_max) b->x_max = x;
                if (y < b->y_min) b->y_min = y;
                if (y > b->y_max) b->y_max = y;
            }
        }
    }

    std::printf("  %-11s %4ux%-4u headline=%u art=%u (w=%u h=%u) detail=%u\n",
                label, w, h, headline.count, art.count, art.width(), art.height(), detail.count);

    check(headline.count > 0, "headline drawn");
    check(art.count > 0, "logo art drawn");
    check(detail.count > 0, "detail line drawn");
    check(all.x_max < w && all.y_max < h, "no writes outside the framebuffer");
    check(art.width() <= w, "logo fits horizontally");
    check(art.height() <= L.detail_top - L.art_top, "logo fits the art band");
    check(art.width() >= w / 3, "logo scaled to fill a good share of the width");
    check(art.x_min >= 1 && art.x_max <= w - 2, "logo centred with margins");

    // Determinism: a second render must be byte-identical.
    Frame g(w, h);
    anko::panic_screen::bind(reinterpret_cast<volatile uint32_t*>(g.px.data()), w, h);
    anko::panic_screen::show("KERNEL PANIC", "panic screen self-test");
    check(f.px == g.px, "re-render is deterministic");
}

} // namespace

int main() {
    run_size(1024, 768, "qemu-ramfb");
    run_size(1280, 720, "t210");
    run_size(1920, 1080, "1080p");
    run_size(640, 480, "small");
    run_size(400, 300, "tiny");

    // A framebuffer too small for even one glyph row must be rejected.
    {
        Frame f(32, 32);
        anko::panic_screen::bind(reinterpret_cast<volatile uint32_t*>(f.px.data()), 32, 32);
        check(!anko::panic_screen::bound(), "tiny framebuffer rejected");
    }

    if (failures != 0) {
        std::printf("panic screen probe FAILED (%d checks)\n", failures);
        return 1;
    }
    std::printf("panic screen probe passed\n");
    return 0;
}

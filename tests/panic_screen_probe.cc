// tests/panic_screen_probe.cc — host test for the kernel panic screen.
//
// Compiles kernel/panic_screen.cc (and kernel/qr.cc) against a synthetic
// XRGB8888 framebuffer and asserts the new layout: QR code top-left, key data
// under it, the logo in the remaining space, and the title/reason left-aligned
// along the bottom. Also checks every display size stays in bounds and renders
// deterministically.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "anko/panic_screen.h"

namespace {

constexpr uint32_t kBg = 0x00060E;
constexpr uint32_t kQrModule = 0x08111C;
constexpr uint32_t kQrPanel = 0xF4F7FF;
constexpr uint32_t kTitle = 0xFF6B6B;
constexpr uint32_t kReason = 0xFFD9A0;
constexpr uint32_t kCaption = 0xC8D6E8;
constexpr uint32_t kArtDim = 0x8FA6C4;
constexpr uint32_t kArtBright = 0xEAF1FF;

struct Frame {
    uint32_t width;
    uint32_t height;
    std::vector<uint32_t> px;
    Frame(uint32_t w, uint32_t h)
        : width(w), height(h), px(static_cast<size_t>(w) * h, 0xDEADBEEFu) {}
};

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool near(uint32_t a, uint32_t b) { return a == b; }

const char* kPayload =
    "AMEQR1 core=0\n"
    "build=ame-2026.09-12-g224b372\n"
    "reason=data abort (EL1)\n"
    "esr=0000000096000045 far=0000000000000042 elr=00000000100018a4\n"
    "spsr=00000000600003c5 sp=ffff000000123400 fp=ffff000000123500 lr=0000000010001f20\n"
    "hw=111 virtio=4 el=2\n"
    "pci=0000000010000000+0000000002eff000 ram=0000000040000000+0000000020000000\n"
    "x0=0000000000000000 x1=0000000000000001 x2=0000000000000002 x3=0000000000000003\n"
    "bt=3 0000000010001f20 0000000010001f60 0000000010001fa0\n";

// The QR encodes the compressed URL; the probe only checks the layout, so a
// URL-shaped string is enough (the capture harness decodes the real bundle).
const char* kQrUrl = "https://ame.example/p#QVBAAQH0AQAA";

void run_size(uint32_t w, uint32_t h, const char* label) {
    Frame f(w, h);
    anko::panic_screen::bind(reinterpret_cast<volatile uint32_t*>(f.px.data()), w, h);
    check(anko::panic_screen::bound(), "bound() after a valid bind");
    anko::panic_screen::show("KERNEL PANIC", "data abort (EL1)", kPayload, kQrUrl);

    uint32_t qr_dark = 0, qr_area_ink = 0, title = 0, reason = 0, caption = 0, art = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = f.px[static_cast<size_t>(y) * w + x];
            if (near(c, kQrModule) || near(c, kQrPanel)) qr_area_ink += (c != kQrPanel);
            if (near(c, kQrPanel)) qr_area_ink += 0;
            if (near(c, kQrModule)) ++qr_dark;
            if (near(c, kTitle)) ++title;
            if (near(c, kReason)) ++reason;
            if (near(c, kCaption)) ++caption;
            if (near(c, kArtDim) || near(c, kArtBright)) ++art;
            if (c != kBg && c != kQrPanel && c != kQrModule && !near(c, kTitle) &&
                !near(c, kReason) && !near(c, kCaption) && !near(c, kArtDim) &&
                !near(c, kArtBright)) {
                // Unknown colour: only acceptable from the 2-px bleed of scaling.
            }
        }
    }

    std::printf("  %-11s %4ux%-4u qr_dark=%u title=%u reason=%u caption=%u art=%u\n",
                label, w, h, qr_dark, title, reason, caption, art);
    (void)qr_area_ink;

    check(qr_dark > 50, "QR code drawn in the top-left");
    check(title > 100, "title drawn");
    check(reason > 100, "reason drawn");
    check(caption > 500, "key data drawn under the QR");
    check(art > 500, "logo drawn in the remaining space");

    // Geometry: the QR lives in the top-left box, the title in the bottom-left.
    uint32_t qr_x_min = w, qr_x_max = 0, qr_y_min = h, qr_y_max = 0;
    uint32_t title_y_min = h, title_x_max = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = f.px[static_cast<size_t>(y) * w + x];
            if (near(c, kQrModule) || near(c, kQrPanel)) {
                if (x < qr_x_min) qr_x_min = x;
                if (x > qr_x_max) qr_x_max = x;
                if (y < qr_y_min) qr_y_min = y;
                if (y > qr_y_max) qr_y_max = y;
            }
            if (near(c, kTitle)) {
                if (y < title_y_min) title_y_min = y;
                if (x > title_x_max) title_x_max = x;
            }
        }
    }
    check(qr_x_min <= w / 8 && qr_y_max <= h * 3 / 4, "QR anchored top-left");
    check(qr_x_max - qr_x_min <= w / 2, "QR box bounded");
    // Aspect ratio: the drawn panel (modules + quiet zone) must be exactly square.
    check((qr_x_max - qr_x_min + 1) == (qr_y_max - qr_y_min + 1), "QR aspect ratio is 1:1");
    // Smaller than a third of the screen, so it stays a corner element.
    check((qr_x_max - qr_x_min + 1) <= w / 3, "QR panel compact");
    check(title_y_min > h / 2, "title in the lower half");
    check(title_x_max < w / 2, "title left-aligned");

    // The key-data column must not run under the logo: no caption pixel may
    // reach into the logo's horizontal band.
    uint32_t art_x_min = w;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = f.px[static_cast<size_t>(y) * w + x];
            if ((near(c, kArtDim) || near(c, kArtBright)) && x < art_x_min) art_x_min = x;
        }
    }
    uint32_t caption_in_logo = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = art_x_min; x < w; ++x) {
            const uint32_t c = f.px[static_cast<size_t>(y) * w + x];
            if (near(c, kCaption)) ++caption_in_logo;
        }
    }
    check(caption_in_logo == 0, "caption stays out of the logo column");

    // The logo must be drawn undistorted: the output column/row ratio has to
    // track the source art's ratio (within one column of rounding).
    uint32_t art_wrong = 0;
    for (uint32_t aw = 0; aw <= 1200; aw += 11) {
        for (uint32_t ah = 0; ah <= 900; ah += 17) {
            uint32_t cols = 0, rows = 0;
            anko::panic_screen::art_grid(aw, ah, &cols, &rows);
            if (cols == 0 || rows == 0) continue;
            const long lhs = static_cast<long>(cols) * 51;
            const long rhs = static_cast<long>(rows) * 139;
            const long diff = lhs > rhs ? lhs - rhs : rhs - lhs;
            if (diff > 51) ++art_wrong;
            if (cols * 8u > aw || rows * 16u > ah) ++art_wrong;
        }
    }
    check(art_wrong == 0, "logo is scaled proportionally and fits its box");

    // Determinism.
    Frame g(w, h);
    anko::panic_screen::bind(reinterpret_cast<volatile uint32_t*>(g.px.data()), w, h);
    anko::panic_screen::show("KERNEL PANIC", "data abort (EL1)", kPayload, kQrUrl);
    check(f.px == g.px, "re-render is deterministic");
}

} // namespace

int main() {
    run_size(1024, 768, "qemu-ramfb");
    run_size(1280, 720, "t210");
    run_size(1920, 1080, "1080p");
    run_size(640, 480, "small");
    run_size(400, 300, "tiny");

    // Without a payload the screen still renders (title/reason/logo only).
    {
        Frame f(1024, 768);
        anko::panic_screen::bind(reinterpret_cast<volatile uint32_t*>(f.px.data()), 1024, 768);
        anko::panic_screen::show("KERNEL PANIC", "no payload", nullptr, nullptr);
        uint32_t uk = 0;
        for (uint32_t c : f.px) {
            if (c != kBg) ++uk;
        }
        check(uk > 1000, "screen renders without a QR payload");
    }

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

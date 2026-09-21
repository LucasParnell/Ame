#pragma once
// anko/panic_screen.h — graphical panic screen.
//
// The kernel owns the framebuffer before init, so a panic can paint a
// self-contained screen: a QR code carrying the panic report (top-left) with
// key data underneath, the project logo (smaller, in the remaining space) and
// the title/reason left-aligned along the bottom. Every routine is
// allocation-free and lock-free so it is safe in any fault or panic context.

#include "anko/types.h"

namespace anko::panic_screen {

// Bind the XRGB8888 framebuffer the panic screen draws into. Called once from
// boot_caps after the ramfb/T210 buffer is live.
void bind(volatile u32* pixels, u32 width, u32 height);

// True once a framebuffer has been bound.
bool bound();

// Cell size for drawing the logo at full size in a box of `avail_w` x `avail_h`
// pixels: every source column and row is drawn (no re-sampling), the glyphs just
// shrink. The cell keeps the source's 8x16 proportions (ch == 2*cw) and is as
// large as fits; both outputs are 0 when even one column would not fit.
void art_cell(u32 avail_w, u32 avail_h, u32* out_cw, u32* out_ch);

// Render the panic screen. `caption` is the readable report text drawn under
// the QR; `qr_payload` is what the QR encodes (the compressed URL from
// panic_report::format_url). Null/empty payloads render the screen without a QR
// code. Returns true when a QR code was drawn (the payload may be too large for
// any version).
bool show(const char* title, const char* reason, const char* caption,
          const char* qr_payload);

} // namespace anko::panic_screen

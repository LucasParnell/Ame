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

// Grid the logo is drawn on for a box of `avail_w` x `avail_h` pixels. The
// glyph grid is scaled *proportionally* (out_cols/art_cols is the same fraction
// as out_rows/art_rows), so the drawn image keeps the source art's aspect ratio
// and is never stretched. Both outputs are 0 when nothing fits.
void art_grid(u32 avail_w, u32 avail_h, u32* out_cols, u32* out_rows);

// Render the panic screen. `caption` is the readable report text drawn under
// the QR; `qr_payload` is what the QR encodes (the compressed URL from
// panic_report::format_url). Null/empty payloads render the screen without a QR
// code. Returns true when a QR code was drawn (the payload may be too large for
// any version).
bool show(const char* title, const char* reason, const char* caption,
          const char* qr_payload);

} // namespace anko::panic_screen

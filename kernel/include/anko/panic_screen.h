#pragma once
// anko/panic_screen.h — graphical panic screen.
//
// The kernel owns the framebuffer before init, so a panic can paint a
// self-contained screen: a QR code carrying the panic report, the readable
// report text, and the project logo. Every routine is allocation-free and
// lock-free so it is safe in any fault or panic context.
//
// Two layouts, chosen by aspect ratio:
//   portrait  (h >= 1.5w)  title/reason on top, QR below them, report text
//                          under the QR, logo across the bottom.
//   landscape              QR top-left, text under it, logo to the right,
//                          title/reason along the bottom.
//
// The panel being scanned out does not always have stride == width: the anne
// DSS window is described by an offset rectangle and a byte stride that can be
// wider than the visible row. bind() therefore takes the stride separately; a
// stride mismatch shears the whole image diagonally.

#include "anko/types.h"

namespace anko::panic_screen {

// Bind the XRGB8888 framebuffer the panic screen draws into. Called once from
// boot_caps after the ramfb/T210/anne buffer is live. `stride` is in pixels and
// must be >= width; the two-argument form assumes stride == width.
void bind(volatile u32* pixels, u32 width, u32 height);
void bind(volatile u32* pixels, u32 width, u32 height, u32 stride);

// Optional: clean the painted range to the point of coherency before returning
// from show(), for panels scanned out of cacheable memory. `base` is the address
// handed to bind(), `bytes` covers the whole strided surface.
void set_flush_hook(void (*fn)(u64 base, u32 bytes));

// Optional: called periodically while painting. The anne panic path points this
// at the watchdog feed so a multi-megabyte fill cannot outlive the timeout it
// inherited from the bootloader.
void set_progress_hook(void (*fn)());

// True once a framebuffer has been bound.
bool bound();

// True once show() has completed. The log console uses this to stop mirroring
// text over the panic screen: a second core panicking, or a late boot
// checkpoint, must not scribble through the QR code.
bool painted();

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
bool show(const char* title, const char* reason, const char* caption, const char* qr_payload);

} // namespace anko::panic_screen

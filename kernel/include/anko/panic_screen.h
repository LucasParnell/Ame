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

// Render the panic screen. `payload` is the QR report text (already rendered by
// panic_report::format_qr); its leading lines are also drawn under the QR as
// plain text. Null/empty payloads render the screen without a QR code.
void show(const char* title, const char* reason, const char* payload);

} // namespace anko::panic_screen

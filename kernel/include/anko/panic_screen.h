#pragma once
// anko/panic_screen.h — graphical panic screen.
//
// The kernel owns the framebuffer before it hands it to io.framebuffer, so a
// panic can paint a self-contained screen: the project logo (an ASCII art asset,
// not a hardcoded blob) scaled to fit, plus a headline and detail line. Every
// routine here is allocation-free and lock-free so it is safe in any fault or
// panic context.

#include "anko/types.h"

namespace anko::panic_screen {

// Bind the XRGB8888 framebuffer the panic screen draws into. Called once from
// boot_caps after the ramfb/T210 buffer is live.
void bind(volatile u32* pixels, u32 width, u32 height);

// True once a framebuffer has been bound.
bool bound();

// Render the panic screen. `headline` defaults to "KERNEL PANIC" when null;
// `detail` (may be null) is drawn as a single line at the bottom. Idempotent.
void show(const char* headline, const char* detail);

} // namespace anko::panic_screen

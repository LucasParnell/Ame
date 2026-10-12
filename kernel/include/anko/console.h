#pragma once
// anko/console.h — per-console driver ops table and dispatch.
//
// console::bind() selects the right driver from g_hw.console_kind and
// initialises the UART at g_hw.console_base.  After bind(), all kernel
// output goes through g_console_ops.

#include "anko/types.h"
#include "anko/hw_info.h"

struct ConsoleOps {
    void (*early_init)(PhysAddr base);
    void (*putc)(char c);
};

namespace console {
// Select driver ops for kind k, call ops->early_init(base), store in
// g_console_ops.  Safe to call multiple times (re-points the UART).
void bind(ConsoleKind k, PhysAddr base);
// The single kernel character-output path, including early logs and panics.
// Dispatch through the bound board driver, then notify the secondary sink.
void putchar(char c);
// Install a secondary character sink (e.g. SD-card log buffer).
// Called once per character after the primary UART putc.
void set_secondary_sink(void (*fn)(char));
} // namespace console

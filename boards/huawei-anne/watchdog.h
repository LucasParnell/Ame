#pragma once
#include "anko/types.h"
namespace anne::watchdog {
#ifdef ANKO_ANNE_DIAG_TEST
constexpr u64 kBase = 0x47500000;
#else
constexpr u64 kBase = 0xe8a06000;
#endif
void adopt();
void poll();
void report(const char* phase);
// Diagnostic retention only: leave hardware running and deliberately stop feeds.
bool pause_for_capture();

// --- panic path -------------------------------------------------------------
// The bootloader arms this SP805 with INTEN|RESEN before handing off, so a core
// that parks in `wfi` is reset by hardware rather than by software. The panic
// handler is the one path that must outlive that deadline, so it owns the
// watchdog explicitly:
//
//   hold()              clear a pending expiry, then reload. Call once, before
//                       any formatting or painting.
//   feed()              unconditional reload, independent of adopt() state and
//                       of the counter-based throttle poll() uses. Safe on any
//                       core, safe before adopt().
//   absorb_interrupt()  clear the SP805 interrupt and report whether it had
//                       fired, so a FIQ can be named instead of reported as an
//                       anonymous "unexpected exception".
void hold();
void feed();
bool absorb_interrupt();
} // namespace anne::watchdog

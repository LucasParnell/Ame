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
}

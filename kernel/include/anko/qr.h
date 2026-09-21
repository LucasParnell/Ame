#pragma once
// anko/qr.h — QR Code Model 2 encoder (byte mode).
//
// Freestanding: no heap, no libc, bounded stack, deterministic. Feature tables
// live in qr_tables.h so the encoder logic stays readable and the spec data can
// be verified independently. Used by the panic screen to carry the trap report
// off a photographed display.

#include "anko/types.h"

namespace anko::qr {

enum class Level : u8 { L = 0, M = 1, Q = 2, H = 3 };

constexpr u32 kMaxVersion = 40;
constexpr u32 kMaxSize = 17 + 4 * kMaxVersion;              // 177 modules
constexpr u32 kMaxCodewords = 3706;                          // v40 total codewords
constexpr u32 kModuleBytes = (kMaxSize * kMaxSize + 7u) / 8; // 3917
constexpr u32 kMaxBlocks = 81;

// A finished code. ~3.9 KB, so callers keep it in static storage, not on the
// stack of a faulting thread.
struct Code {
    u32 version;                    // 1..40
    u32 size;                       // modules per side = 17 + 4*version
    u32 mask;                       // 0..7
    u8  modules[kModuleBytes];      // row-major, MSB-first; bit set = dark
};

// Smallest version that fits `len` bytes at `level`, or 0 if none does.
u32 fit_version(u32 len, Level level);

// Encode `data` in byte mode at `level`. Returns false when it does not fit.
bool encode(const u8* data, u32 len, Level level, Code& out);

// Module state. Coordinates outside the symbol read as light, which is what a
// renderer wants for the quiet zone.
bool dark(const Code& code, u32 x, u32 y);

} // namespace anko::qr

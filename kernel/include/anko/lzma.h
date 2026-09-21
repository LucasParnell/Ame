#pragma once
// anko/lzma.h — minimal LZMA1 encoder (raw stream, no container).
//
// Enough of LZMA to compress the panic bundle for a QR URL: a binary range
// coder, the literal/match coders and the distance coder, with greedy matching.
// Parameters are fixed and documented so a decoder can be written against them
// (Python's `lzma` with FORMAT_RAW and the same filter decodes it):
//
//   lc = 3, lp = 0, pb = 2, dict_size = 4096, no end-of-stream marker
//   (the caller stores the uncompressed length, so the decoder knows when to
//   stop — see panic_report's bundle header).
//
// Allocation-free: all probability tables live in static storage. The input for
// the panic path is on the order of a few hundred bytes, so matching is a plain
// bounded search rather than a hash chain.

#include "anko/types.h"

namespace anko::lzma {

constexpr u32 kLc = 3;
constexpr u32 kLp = 0;
constexpr u32 kPb = 2;
constexpr u32 kDictSize = 4096;
constexpr u32 kMinMatch = 2;
constexpr u32 kMaxMatch = 273;

// Worst-case bound for `bound(input_len)` bytes of output.
u32 bound(u32 input_len);

// Compress `len` bytes. Returns the compressed size, or 0 when the output would
// not fit `cap` (the caller then stores the data uncompressed).
u32 compress(const u8* data, u32 len, u8* out, u32 cap);

} // namespace anko::lzma

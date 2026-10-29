// lib/libk/string.cc — freestanding string / memory helpers.
//
// Phase 0 (spec §12): no host libc. The compiler may also emit calls to
// memcpy/memset/memmove for aggregate copies and zero-init, so we expose
// extern "C" wrappers with the standard C signatures.

#include "libk/string.h"
#include "libk/str.h"
#include "libk/bytes.h"

namespace libk {

void* memset(void* dst, int c, size_t n) {
    auto* p = static_cast<unsigned char*>(dst);
    auto v = static_cast<unsigned char>(c);
    for (size_t i = 0; i < n; ++i) p[i] = v;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    auto* pa = static_cast<const unsigned char*>(a);
    auto* pb = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return int(pa[i]) - int(pb[i]);
    }
    return 0;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { ++a; ++b; }
    return int(static_cast<unsigned char>(*a)) - int(static_cast<unsigned char>(*b));
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca != cb) return int(ca) - int(cb);
        if (ca == 0)  return 0;
    }
    return 0;
}

size_t strlen(const char* s) {
    // One implementation of the length scan lives in libk/str.h.
    return libk::str::length(s);
}

uint32_t be32(const void* p) {
    auto* b = static_cast<const unsigned char*>(p);
    return  (uint32_t(b[0]) << 24)
          | (uint32_t(b[1]) << 16)
          | (uint32_t(b[2]) <<  8)
          |  uint32_t(b[3]);
}

uint64_t be64(const void* p) {
    auto* b = static_cast<const unsigned char*>(p);
    return  (uint64_t(b[0]) << 56)
          | (uint64_t(b[1]) << 48)
          | (uint64_t(b[2]) << 40)
          | (uint64_t(b[3]) << 32)
          | (uint64_t(b[4]) << 24)
          | (uint64_t(b[5]) << 16)
          | (uint64_t(b[6]) <<  8)
          |  uint64_t(b[7]);
}

} // namespace libk

// Compiler-emitted symbols. GCC/Clang are allowed to emit these in
// freestanding mode for struct copies, zero-init, and overlapping moves.
extern "C" void* memset(void* dst, int c, size_t n)              { return libk::memset(dst, c, n); }
extern "C" void* memcpy(void* dst, const void* src, size_t n)    { return libk::memcpy(dst, src, n); }
extern "C" int   memcmp(const void* a, const void* b, size_t n)  { return libk::memcmp(a, b, n); }

extern "C" void* memmove(void* dst, const void* src, size_t n) {
    (void)libk::move_n(libk::as_bytes(dst, n), libk::as_bytes(src, n), n);
    return dst;
}

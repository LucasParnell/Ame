#pragma once
// libk/string.h — INTERNAL runtime floor: untyped memory + C-string primitives.
//
// These are the freestanding equivalents of the C library's <string.h>. The
// memory functions (memcpy/memset/memmove/memcmp) take an untyped (pointer,
// length) pair and trust the caller completely; the compiler also emits calls to
// them for aggregate copies and zero-init, so the symbols must exist in every
// image.
//
// They are deliberately NOT part of the caller-facing API. Services use
// libk/bytes.h (sized views, bounds-checked) and libk/str.h instead; the build
// defines AME_MOCHI_SERVICE for every service, so a service that reaches for the
// raw floor is a build error — which is the point.
#if defined(AME_MOCHI_SERVICE)
#error "libk/string.h is the internal runtime floor (raw memcpy/memset/memmove), \
which services must not use. Use libk/bytes.h (libk::copy / libk::copy_n / \
libk::fill) for memory and libk/str.h for strings."
#endif

#include <cstddef>
#include <cstdint>

namespace libk {

void*  memset(void* dst, int c, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
size_t strlen(const char* s);

// Big-endian read helpers — DTB is big-endian.
uint32_t be32(const void* p);
uint64_t be64(const void* p);

} // namespace libk

#pragma once
// libk/str.h — C-string helpers with explicit destination capacities.
//
// length/equal/starts_with require NUL-terminated input. Use the _n forms for
// unterminated buffers. Copy functions bound writes by the destination capacity;
// the source must be readable up to its NUL or the stated scan limit.
//
// Freestanding: <cstddef> only.

#include <cstddef>

namespace libk::str {

// Length up to (but not including) the first NUL. Null is the empty string.
inline std::size_t length(const char* s) {
    if (!s) return 0;
    std::size_t n = 0;
    while (s[n]) ++n;
    return n;
}

// Length capped at `max` — use when `s` may not be terminated within its buffer.
inline std::size_t length_n(const char* s, std::size_t max) {
    if (!s) return 0;
    std::size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

inline bool equal(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    std::size_t i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return a[i] == b[i];
}

// Equality of at most `n` bytes; a NUL in both terminates early.
inline bool equal_n(const char* a, const char* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char ca = a ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = b ? static_cast<unsigned char>(b[i]) : 0;
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
    return true;
}

inline bool starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    std::size_t i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return false;
        ++i;
    }
    return true;
}

// Character classes for the shell/parsers.
inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_ascii_printable(char c) { return c >= 32 && c <= 126; }

// Copy `src` into dst[0..cap) with a NUL terminator when it fits; truncates
// otherwise. Returns characters copied (excluding the terminator). Never writes
// past `cap`; cap == 0 writes nothing. Reads at most cap - 1 source characters.
inline std::size_t copy_into(char* dst, std::size_t cap, const char* src) {
    if (!dst || cap == 0) return 0;
    std::size_t i = 0;
    if (src) {
        while (i + 1 < cap && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
    return i;
}

// Checked replacement for strcpy: false (dst untouched) unless `src` fits with
// its terminator. Reads at most cap source characters.
inline bool copy_exact_fit(char* dst, std::size_t cap, const char* src) {
    if (!dst || cap == 0) return false;
    const std::size_t n = length_n(src, cap);
    if (n == cap) return false;
    for (std::size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = '\0';
    return true;
}

} // namespace libk::str

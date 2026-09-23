#pragma once
// libk/num.h — bounded integer <-> text helpers.
//
// Writers take the destination capacity and never write past it (truncating
// with a terminator); parsers consume the whole run and report overflow instead
// of wrapping.
//
// Freestanding: <cstddef> + <cstdint> only.

#include <cstddef>
#include <cstdint>

namespace libk::num {

// Parse a decimal or 0x-prefixed hex unsigned integer. False on any stray
// character, an empty run, or a value that would overflow a u64.
inline bool parse_u64(const char* s, std::uint64_t* out) {
    if (!s) return false;
    std::size_t i = 0;
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    }
    std::uint64_t value = 0;
    bool any = false;
    for (; s[i]; ++i) {
        const char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = 10u + static_cast<unsigned>(c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') d = 10u + static_cast<unsigned>(c - 'A');
        else return false;
        // Reject value*base + d rather than silently wrapping.
        if (value > (0xFFFFFFFFFFFFFFFFull - d) / base) return false;
        value = value * base + d;
        any = true;
    }
    if (!any) return false;
    if (out) *out = value;
    return true;
}

// Write `value` in decimal into dst[0..cap). Returns characters written
// (excluding the terminator); a short capacity truncates but still terminates.
inline std::size_t to_dec(std::uint64_t value, char* dst, std::size_t cap) {
    if (!dst || cap == 0) return 0;
    char tmp[24];
    std::size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value && n < sizeof(tmp));
    std::size_t j = 0;
    while (n && j + 1 < cap) dst[j++] = tmp[--n];
    dst[j] = '\0';
    return j;
}

// Write `value` in lower-case hex with a 0x prefix, as the shell prints them.
inline std::size_t to_hex(std::uint64_t value, char* dst, std::size_t cap) {
    if (!dst || cap == 0) return 0;
    static const char kDigits[] = "0123456789abcdef";
    std::size_t n = 0;
    const auto put = [&](char c) { if (n + 1 < cap) dst[n++] = c; };
    put('0');
    put('x');
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned d = static_cast<unsigned>((value >> shift) & 0xFu);
        if (d || started || shift == 0) {
            started = true;
            put(kDigits[d]);
        }
    }
    dst[n] = '\0';
    return n;
}

} // namespace libk::num

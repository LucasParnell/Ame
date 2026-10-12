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
#include "libk/str.h"

namespace libk::num {

// Parse exactly size digits in radix 2..36, without signs, prefixes or spaces.
// On failure, leave out untouched. The bounded form accepts unterminated input.
inline bool parse_u64_digits(const char* text, std::size_t size, unsigned base,
                             std::uint64_t* out) {
    if (!text || !size || base < 2 || base > 36) return false;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'z') digit = 10u + static_cast<unsigned>(c - 'a');
        else if (c >= 'A' && c <= 'Z') digit = 10u + static_cast<unsigned>(c - 'A');
        else return false;
        if (digit >= base || value > (~std::uint64_t{0} - digit) / base) return false;
        value = value * base + digit;
    }
    if (out) *out = value;
    return true;
}

// Decimal or 0x-prefixed hexadecimal, consuming exactly size characters.
inline bool parse_u64(const char* text, std::size_t size, std::uint64_t* out) {
    if (!text) return false;
    const bool hex = size >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    return parse_u64_digits(text + (hex ? 2 : 0), size - (hex ? 2 : 0), hex ? 16 : 10, out);
}

// C-string convenience form. Input must be NUL-terminated.
inline bool parse_u64(const char* text, std::uint64_t* out) {
    return parse_u64(text, libk::str::length(text), out);
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

#pragma once
// Bounded CTAP canonical CBOR. No heap, indefinite lengths, tags or floats.
#include "midix/abi/types.h"
#include "libk/bytes.h"
#include "libk/str.h"
namespace mochi::fido2 {
struct Value {
    const mx_u8_t* data = nullptr;
    mx_u32_t size = 0, head = 0;
    mx_u64_t number = 0;
    mx_u8_t major = 0;
    explicit operator bool() const { return data; }
    libk::const_bytes bytes() const { return {data + head, static_cast<mx_size_t>(number)}; }
    bool text(const char* s) const {
        const auto n = libk::str::length(s);
        if (major != 3 || number != n) return false;
        for (mx_u32_t i = 0; i < n; ++i) if (data[head + i] != static_cast<mx_u8_t>(s[i])) return false;
        return true;
    }
};
inline bool parse(const mx_u8_t* data, mx_u32_t size, Value& out, mx_u32_t depth = 0) {
    if (!size || depth > 8) return false;
    const auto major = data[0] >> 5, additional = data[0] & 31;
    mx_u32_t head = 1;
    mx_u64_t number = additional;
    if (additional >= 24) {
        if (additional > 27) return false;
        const mx_u32_t count = 1u << (additional - 24);
        head += count;
        if (size < head) return false;
        number = 0;
        for (mx_u32_t i = 1; i < head; ++i) number = (number << 8) | data[i];
        if (number < 24 || (count > 1 && number < (mx_u64_t{1} << (count / 2 * 8)))) return false;
    }
    mx_u32_t length = head;
    if (major == 2 || major == 3) {
        if (number > size - head) return false;
        length += number;
        // UTF-8 text must be well formed; reject overlongs and surrogate scalars.
        if (major == 3) {
            for (mx_u32_t i = head; i < length;) {
                auto c = data[i++];
                if (c < 128) continue;
                mx_u32_t n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2
                                                   : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
                if (!n || n > length - i) return false;
                mx_u32_t scalar = c & ((1u << (6 - n)) - 1);
                for (mx_u32_t j = 0; j < n; ++j) {
                    c = data[i++];
                    if ((c & 0xc0) != 0x80) return false;
                    scalar = (scalar << 6) | (c & 63);
                }
                if (scalar < (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) ||
                    scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
            }
        }
    } else if (major == 4 || major == 5) {
        if (number > size / (major == 5 ? 2 : 1)) return false;
        Value previous{};
        for (mx_u64_t i = 0; i < number * (major == 5 ? 2 : 1); ++i) {
            Value child{};
            if (!parse(data + length, size - length, child, depth + 1)) return false;
            if (major == 5 && !(i & 1)) {
                if (child.major != 0 && child.major != 1 && child.major != 3) return false;
                if (previous) {
                    // Canonical length-first key ordering also forbids duplicates.
                    if (child.size < previous.size) return false;
                    if (child.size == previous.size) {
                        mx_u32_t j = 0;
                        while (j < child.size && previous.data[j] == child.data[j]) ++j;
                        if (j == child.size || previous.data[j] > child.data[j]) return false;
                    }
                }
                previous = child;
            }
            length += child.size;
        }
    } else if (major > 1 && !(major == 7 && (additional == 20 || additional == 21))) return false;
    out = {data, length, head, number, static_cast<mx_u8_t>(major)};
    return true;
}
inline Value member(Value map, mx_u64_t key, const char* text = nullptr) {
    if (!map || map.major != 5) return {};
    mx_u32_t at = map.head;
    for (mx_u64_t i = 0; i < map.number; ++i) {
        Value k{}, v{};
        if (!parse(map.data + at, map.size - at, k)) return {};
        at += k.size;
        if (!parse(map.data + at, map.size - at, v)) return {};
        at += v.size;
        if (text ? k.text(text) : k.major == 0 && k.number == key) return v;
    }
    return {};
}
inline Value element(Value array, mx_u32_t index) {
    if (!array || array.major != 4 || index >= array.number) return {};
    mx_u32_t at = array.head;
    Value child{};
    for (mx_u32_t i = 0; i <= index; ++i) {
        if (!parse(array.data + at, array.size - at, child)) return {};
        at += child.size;
    }
    return child;
}
struct Writer {
    libk::bytes output;
    mx_u32_t size = 0;
    bool good = true;
    void byte(mx_u8_t b) {
        if (size < output.size) output.data[size++] = b;
        else good = false;
    }
    void raw(libk::const_bytes b) {
        if (b.size > output.size - size) { good = false; return; }
        libk::copy_n(libk::slice(output, size, b.size), b, b.size);
        size += b.size;
    }
    void item(mx_u8_t major, mx_u64_t n) {
        if (n < 24) { byte((major << 5) | n); return; }
        const mx_u32_t count = n <= 255 ? 1 : n <= 65535 ? 2 : n <= 0xffffffff ? 4 : 8;
        byte((major << 5) | (count == 1 ? 24 : count == 2 ? 25 : count == 4 ? 26 : 27));
        for (mx_u32_t i = count; i; --i) byte(n >> ((i - 1) * 8));
    }
    void uint(mx_u64_t n) { item(0, n); }
    void text(const char* s) { const auto n = libk::str::length(s); item(3, n); raw(libk::as_bytes(s, n)); }
    void bytes(libk::const_bytes b) { item(2, b.size); raw(b); }
    void map(mx_u32_t n) { item(5, n); }
    void array(mx_u32_t n) { item(4, n); }
    void boolean(bool v) { byte(v ? 0xf5 : 0xf4); }
};
} // namespace mochi::fido2

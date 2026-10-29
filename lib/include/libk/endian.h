#pragma once
// Unaligned little-endian integers, decoded bytewise (no aliasing or alignment
// assumptions). Use read_le for wire buffers; load_le requires sizeof(T) readable
// bytes and is intended for fields inside an already validated fixed layout.
#include "libk/bytes.h"

namespace libk {

template <typename T>
inline bool read_le(const_bytes source, T* out) {
    static_assert(__is_integral(T) && __is_unsigned(T), "endian reads require an unsigned integer");
    if (!out || !source.data || source.size < sizeof(T)) return false;
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(source.data[i]) << (i * 8);
    }
    *out = value;
    return true;
}

template <typename T>
inline T load_le(const void* source) {
    T value = 0;
    (void)read_le(as_bytes(source, sizeof(T)), &value);
    return value;
}

// Refuses a short destination before changing any byte.
template <typename T>
inline bool write_le(bytes destination, T value) {
    static_assert(__is_integral(T) && __is_unsigned(T), "endian writes require an unsigned integer");
    if (!destination.data || destination.size < sizeof(T)) return false;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        destination.data[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
    return true;
}

} // namespace libk

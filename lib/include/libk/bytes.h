#pragma once
// libk/bytes.h — bounded, sized byte helpers.
//
// This is the sanctioned way for libk/mochi/native code to move bytes. The raw
// memcpy/memset in libk/string.h are the internal runtime floor: they take an
// untyped (pointer, length) pair and trust the caller completely. These helpers
// take a sized (pointer, size) view and cannot read or write outside it, so a
// wrong length becomes a returned count or a false — never an out-of-bounds
// access.
//
// Freestanding: <cstddef> + <cstdint> only.

#include <cstddef>
#include <cstdint>

namespace libk {

// A mutable byte view. A null `data` always carries size 0 (see as_bytes).
struct bytes {
    std::uint8_t* data;
    std::size_t size;
};

// A read-only byte view.
struct const_bytes {
    const std::uint8_t* data;
    std::size_t size;
};

// Build a view. (nullptr, n) normalises to the empty view so the helpers below
// never dereference a null pointer.
inline bytes as_bytes(void* data, std::size_t size) {
    return {data ? static_cast<std::uint8_t*>(data) : nullptr, data ? size : std::size_t{0}};
}
inline const_bytes as_bytes(const void* data, std::size_t size) {
    return {data ? static_cast<const std::uint8_t*>(data) : nullptr, data ? size : std::size_t{0}};
}

// Sub-view [offset, offset+length). A range outside `b` yields the empty view.
inline const_bytes slice(const_bytes b, std::size_t offset, std::size_t length) {
    if (offset > b.size || length > b.size - offset) return {nullptr, 0};
    return {b.data + offset, length};
}
inline bytes slice(bytes b, std::size_t offset, std::size_t length) {
    if (offset > b.size || length > b.size - offset) return {nullptr, 0};
    return {b.data + offset, length};
}

// Copy min(dst.size, src.size) bytes; returns the number copied.
inline std::size_t copy(bytes dst, const_bytes src) {
    const std::size_t n = dst.size < src.size ? dst.size : src.size;
    for (std::size_t i = 0; i < n; ++i) dst.data[i] = src.data[i];
    return n;
}

// Copy exactly when the two views are the same size; false (nothing written)
// otherwise, so a truncated copy can never be mistaken for a full one.
inline bool copy_exact(bytes dst, const_bytes src) {
    if (dst.size != src.size) return false;
    for (std::size_t i = 0; i < dst.size; ++i) dst.data[i] = src.data[i];
    return true;
}

// Checked replacement for memcpy(dst, src, n): refuses unless both views hold
// at least `n` bytes.
inline bool copy_n(bytes dst, const_bytes src, std::size_t n) {
    if (n > dst.size || n > src.size) return false;
    for (std::size_t i = 0; i < n; ++i) dst.data[i] = src.data[i];
    return true;
}

// Checked replacement for memset(dst, value, n): refuses unless dst holds `n`.
inline bool fill_n(bytes dst, std::uint8_t value, std::size_t n) {
    if (n > dst.size) return false;
    for (std::size_t i = 0; i < n; ++i) dst.data[i] = value;
    return true;
}

// Fill the whole view.
inline void fill(bytes dst, std::uint8_t value) {
    for (std::size_t i = 0; i < dst.size; ++i) dst.data[i] = value;
}

} // namespace libk

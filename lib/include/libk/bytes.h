#pragma once
// libk/bytes.h — bounded, sized byte helpers.
//
// This is the sanctioned way for libk/mochi/native code to move bytes. The raw
// memcpy/memset in libk/string.h are the internal runtime floor: they take an
// untyped (pointer, length) pair and trust the caller completely. These helpers
// check lengths against a sized view. The caller must supply the actual extent
// of live storage; a view cannot validate the allocation behind its pointer.
// Copies allow identical source/destination ranges, but not partial overlap.
//
// Freestanding: <cstddef> + <cstdint> only.

#include <cstddef>
#include <cstdint>

namespace libk {

// Non-owning views. as_bytes normalises null pointers to size zero; operations
// also reject nonempty null views constructed directly by a caller.
struct bytes {
    std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// A read-only byte view. Constructors are explicit about the two shapes a
// caller holds, and a mutable view converts to a read-only one for free.
struct const_bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    constexpr const_bytes() = default;
    constexpr const_bytes(const std::uint8_t* d, std::size_t s) : data(d), size(s) {}
    constexpr const_bytes(bytes b) : data(b.data), size(b.size) {} // mutable -> read-only
};

// Build a view. (nullptr, n) normalises to the empty view so the helpers below
// never dereference a null pointer.
inline bytes as_bytes(void* data, std::size_t size) {
    return {data ? static_cast<std::uint8_t*>(data) : nullptr, data ? size : std::size_t{0}};
}
inline const_bytes as_bytes(const void* data, std::size_t size) {
    return {data ? static_cast<const std::uint8_t*>(data) : nullptr, data ? size : std::size_t{0}};
}

// Array extents come from the type rather than a repeated caller-supplied size.
template <typename T, std::size_t N>
inline bytes as_bytes(T (&data)[N]) {
    static_assert(__is_trivially_copyable(T), "byte views require trivially copyable elements");
    return as_bytes(data, sizeof(data));
}
template <typename T, std::size_t N>
inline const_bytes as_bytes(const T (&data)[N]) {
    static_assert(__is_trivially_copyable(T), "byte views require trivially copyable elements");
    return as_bytes(data, sizeof(data));
}

// Sub-view [offset, offset+length). A range outside `b` yields the empty view.
inline const_bytes slice(const_bytes b, std::size_t offset, std::size_t length) {
    if (!b.data || offset > b.size || length > b.size - offset) return {nullptr, 0};
    return {b.data + offset, length};
}
inline bytes slice(bytes b, std::size_t offset, std::size_t length) {
    if (!b.data || offset > b.size || length > b.size - offset) return {nullptr, 0};
    return {b.data + offset, length};
}

// Checked replacement for memcpy(dst, src, n): refuses unless both views hold
// at least `n` bytes.
inline bool copy_n(bytes dst, const_bytes src, std::size_t n) {
    if (n > dst.size || n > src.size || (n != 0 && (!dst.data || !src.data))) return false;
    for (std::size_t i = 0; i < n; ++i) dst.data[i] = src.data[i];
    return true;
}

// Bounded memmove, including partial overlap. Aligned bulk moves use whole
// words; byte tails and unaligned ranges retain the same overlap semantics.
inline bool move_n(bytes dst, const_bytes src, std::size_t n) {
    if (n > dst.size || n > src.size || (n && (!dst.data || !src.data))) return false;
    if (!n || dst.data == src.data) return true;
    const auto dest = reinterpret_cast<std::uintptr_t>(dst.data);
    const auto source = reinterpret_cast<std::uintptr_t>(src.data);
    const bool backward = dest > source && dest - source < n;
    using Word = std::uint64_t __attribute__((__may_alias__));
    if (((dest | source | n) & (sizeof(Word) - 1)) == 0) {
        auto* d = reinterpret_cast<Word*>(dst.data);
        const auto* s = reinterpret_cast<const Word*>(src.data);
        const auto count = n / sizeof(Word);
        if (backward) {
            for (std::size_t i = count; i; --i) d[i - 1] = s[i - 1];
        } else {
            for (std::size_t i = 0; i < count; ++i) d[i] = s[i];
        }
    } else if (backward) {
        for (std::size_t i = n; i; --i) dst.data[i - 1] = src.data[i - 1];
    } else {
        for (std::size_t i = 0; i < n; ++i) dst.data[i] = src.data[i];
    }
    return true;
}

// Copy min(dst.size, src.size) bytes; returns zero for a nonempty null view.
inline std::size_t copy(bytes dst, const_bytes src) {
    const std::size_t n = dst.size < src.size ? dst.size : src.size;
    return copy_n(dst, src, n) ? n : 0;
}

// Copy exactly when the two views are the same size; false (nothing written)
// otherwise, so a truncated copy can never be mistaken for a full one.
inline bool copy_exact(bytes dst, const_bytes src) {
    return dst.size == src.size && copy_n(dst, src, dst.size);
}

// Checked replacement for memset(dst, value, n): refuses unless dst holds `n`.
inline bool fill_n(bytes dst, std::uint8_t value, std::size_t n) {
    if (n > dst.size || (n != 0 && !dst.data)) return false;
    for (std::size_t i = 0; i < n; ++i) dst.data[i] = value;
    return true;
}

// Fill the whole view; a null view is a no-op.
inline void fill(bytes dst, std::uint8_t value) {
    (void)fill_n(dst, value, dst.size);
}

// Inspect a complete range without reading beyond its supplied extent.
inline bool is_zero(const_bytes src) {
    if (src.size && !src.data) return false;
    for (std::size_t i = 0; i < src.size; ++i) {
        if (src.data[i] != 0) return false;
    }
    return true;
}

// Clear storage byte-for-byte, including padding. This is not value
// initialisation: the caller's data format must permit an all-zero representation.
// Like fill_n, a nonempty null view returns false without writing anything.
inline bool zero(bytes dst) {
    return fill_n(dst, 0, dst.size);
}

// Dynamic storage: the caller supplies the actual writable extent.
inline bool zero(void* data, std::size_t size) {
    return zero(bytes{static_cast<std::uint8_t*>(data), size});
}

// Arrays and objects carry their own extent. A bare pointer is deliberately
// rejected: pass *ptr to clear the object, or (ptr, size) for dynamic storage.
template <typename T>
inline bool zero(T& object) {
    static_assert(__is_trivially_copyable(T), "zero requires trivially copyable storage");
    static_assert(!__is_pointer(T), "zero a pointee or provide the storage size");
    return zero(static_cast<void*>(&object), sizeof(object));
}

} // namespace libk

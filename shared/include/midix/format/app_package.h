#pragma once
#include <stddef.h>
#include <stdint.h>

// AMEAPP v1 is a bounded, little-endian container, not an authority or signature.
// Decode bytes explicitly: callers may supply unaligned or untrusted input.
namespace midix::package {
constexpr uint32_t kMaxBytes = 8 * 1024 * 1024, kMaxEntries = 64;
constexpr uint32_t kHeaderBytes = 32, kEntryBytes = 80, kNameBytes = 64;
struct Bytes {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    explicit operator bool() const { return data != nullptr; }
};
inline uint64_t read(const uint8_t* p, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < count; ++i) value |= uint64_t{p[i]} << (i * 8);
    return value;
}
inline bool equal(const uint8_t* p, const char* name) {
    for (uint32_t i = 0; i < kNameBytes; ++i) {
        if (p[i] != static_cast<uint8_t>(name[i])) return false;
        if (!p[i]) return true;
    }
    return false;
}
inline bool has_magic(const uint8_t* data, uint64_t size) {
    constexpr uint8_t magic[] = {'A', 'M', 'E', 'A', 'P', 'P', '1', 0};
    if (!data || size < sizeof(magic)) return false;
    for (uint32_t i = 0; i < sizeof(magic); ++i)
        if (data[i] != magic[i]) return false;
    return true;
}
inline bool valid_name(const uint8_t* name) {
    uint32_t segment = 0;
    for (uint32_t i = 0; i < kNameBytes; ++i) {
        const auto c = name[i];
        if (!c) {
            if (!segment) return false;
            for (; i < kNameBytes; ++i) if (name[i]) return false;
            return true;
        }
        if (c == '/') {
            if (!segment) return false;
            segment = 0;
        } else {
            // Dot-prefixed components (including . and ..) are disallowed.
            if ((!segment && c == '.') ||
                !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return false;
            ++segment;
        }
    }
    return false;
}
struct View {
    const uint8_t* data = nullptr;
    uint32_t count = 0;
    Bytes find(const char* name) const {
        if (!data || !name) return {};
        for (uint32_t i = 0; i < count; ++i) {
            const auto* entry = data + kHeaderBytes + i * kEntryBytes;
            if (equal(entry, name))
                return {data + read(entry + 64, 8), static_cast<uint32_t>(read(entry + 72, 8))};
        }
        return {};
    }
    Bytes executable() const { return find("app.mxi"); }
};
inline bool parse(const uint8_t* data, uint64_t size, View& out) {
    out = {};
    if (!has_magic(data, size) || size < kHeaderBytes || size > kMaxBytes ||
        read(data + 8, 4) != 1 || read(data + 16, 8) != size || read(data + 24, 8))
        return false;
    const auto count = read(data + 12, 4);
    if (!count || count > kMaxEntries) return false;
    uint64_t cursor = kHeaderBytes + count * kEntryBytes;
    if (cursor > size) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* entry = data + kHeaderBytes + i * kEntryBytes;
        const auto offset = read(entry + 64, 8), length = read(entry + 72, 8);
        if (!valid_name(entry) || offset != cursor || length > size - cursor ||
            (!i && (!equal(entry, "app.mxi") || !length))) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (equal(data + kHeaderBytes + j * kEntryBytes,
                      reinterpret_cast<const char*>(entry))) return false;
        cursor += length;
    }
    if (cursor != size) return false;
    out = {data, static_cast<uint32_t>(count)};
    return true;
}
} // namespace midix::package

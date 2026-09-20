#pragma once
#include "midix/abi/image.h"
namespace midix::format {
constexpr mx_u32_t kMaxObjects = 64;
struct Bundle {
    MxBundleHeader header{};
    const mx_u8_t* data = nullptr;
    mx_size_t size = 0;
    const char* manifest = nullptr;
    mx_size_t manifest_size = 0;
};
// Return nullptr on success, otherwise a stable diagnostic. All checks precede
// publication of output; callers must never fall back after rejection.
const char* verify_bundle(const mx_u8_t* data, mx_size_t size, const mx_u8_t (*keys)[32],
                          mx_size_t key_count, Bundle& out);
bool object(const Bundle& bundle, mx_u32_t index, MxBundleObject& out, const char*& name);
} // namespace midix::format

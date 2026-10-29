#pragma once
#include "anko/types.h"

namespace mali {
// Midgard page-table entry fields, which the mediator and the shadow builder
// must agree on. Panfrost formats entries with ARM_MALI_LPAE, not plain ARM
// LPAE, so the attribute bits are carried over verbatim from the guest entry
// while the address half is rebuilt from a host translation.
//   bits  0..1    entry type: 3 = table, 1 = leaf (including 4 KiB pages)
//   bits  2..11   attributes shared with the guest entry
//   bits 12..51   output physical address
constexpr u64 kPteAddressMask = 0x000000fffffff000ULL;
constexpr u64 kPteAttributeMask = (0x3ffULL << 2) | (1ULL << 54);
constexpr u64 kPteValid = 3;
constexpr u64 kPteLeaf = 1;
// ARM_MALI_LPAE uses stage-2-like HAP permissions: bit 6 reads, bit 7 writes.
constexpr u64 kPteWrite = 1ULL << 7;
// One low/high word of the 64-bit translation-table base register.
constexpr u64 kWordMask = 0xffffffffULL;

// Only a 32-bit GPU VA is advertised to Linux. Host-owned shadow tables are
// never placed in GPU-visible RAM. The host translates every leaf from an
// owned guest RAM page; the physical GPU never walks a guest-supplied table.
constexpr u32 kShadowMaxTables = 512;
constexpr u32 kShadowMaxLeaves = 32768;
struct ShadowOps {
    void* ctx;
    bool (*read)(void*, u64 guest_address, u64*);
    bool (*translate)(void*, u64 guest_page, bool writable, u64* host_page);
    u64* (*allocate)(void*, u64* host_address);
    void (*release)(void*, u64*);
    void (*publish)(void*, u64*, u32 bytes);
    // Optional owned-RAM table mapping. Validate/translate the table page once;
    // entries are still loaded atomically and every leaf is fully authorized.
    const u64* (*map_table)(void*, u64 guest_page) = nullptr;
};
struct Shadow {
    u64* tables[kShadowMaxTables];
    u64 physical[kShadowMaxTables];
    u64 root;
    u32 count, leaves, capacity, reused, published;
};
// Build into an empty, unpublished tree. Failure destroys only this new tree;
// an active tree must remain pinned until hardware flush/reset is acknowledged.
bool shadow_build(const ShadowOps&, u64 guest_root, Shadow&);
// Only recycle a tree after the hardware has stopped walking it.
void shadow_recycle(Shadow&);
void shadow_destroy(const ShadowOps&, Shadow&);
} // namespace mali

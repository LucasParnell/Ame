#pragma once
// Generic process BootInfo page ABI.
// Slot 0 in every spawned service is an arena containing this page. Every
// other startup capability is named here; fixed cap layouts are legacy only.

#include "midix/abi/types.h"

constexpr mx_u32_t MX_BOOT_MAGIC = 0x424F4F54u; // 'BOOT'
constexpr mx_u16_t MX_BOOT_VERSION = 2;
constexpr mx_u32_t MX_BOOT_NAME_MAX = 32;
constexpr mx_u32_t MX_BOOT_MAX_ENTRIES = 48;

enum MxBootEntryType : mx_u32_t {
    MX_BOOT_ENTRY_NONE = 0,
    MX_BOOT_ENTRY_HANDLE = 1,
    MX_BOOT_ENTRY_RING = 2,
    MX_BOOT_ENTRY_DOORBELL = 3,
    MX_BOOT_ENTRY_MMIO = 4,
    MX_BOOT_ENTRY_ARENA = 5,
    MX_BOOT_ENTRY_ENDPOINT = 6,
    MX_BOOT_ENTRY_VALUE = 7,
};

struct MxBootEntry {
    mx_u32_t type;
    mx_u32_t flags;
    mx_handle_t handle;
    mx_u64_t aux0;
    mx_u64_t aux1;
    char name[MX_BOOT_NAME_MAX];
};
static_assert(sizeof(MxBootEntry) == 64, "boot entry must stay compact/cache-line sized");

struct MxBootInfo {
    mx_u32_t magic;
    mx_u16_t version;
    mx_u16_t entry_count;
    mx_handle_t self_process;
    mx_handle_t nameservice;
    mx_handle_t supervisor;
    mx_u64_t reserved0;
    MxBootEntry entries[MX_BOOT_MAX_ENTRIES];
};
static_assert(sizeof(MxBootInfo) <= 4096, "boot info must fit in one page");

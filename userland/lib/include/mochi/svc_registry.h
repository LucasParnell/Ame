#pragma once
// mochi/svc_registry.h — tiny read-only service registry page shared by init.
//
// This is deliberately not a service-manager protocol.  It gives early tools
// such as msh enough boot-time visibility to render `svc list/tree/deps` while
// preserving the current Midix model: init owns supervision policy and service
// authority is not ambient.

#include "midix/abi/types.h"

namespace mochi::svc {

constexpr mx_u32_t kRegistryMagic = 0x53564352u; // 'SVCR'
constexpr mx_u16_t kRegistryVersion = 1;
constexpr mx_u32_t kMaxRegistryEntries = 24;

constexpr mx_u32_t kStateSpawned = 1u << 0;
constexpr mx_u32_t kStateBootPublished = 1u << 1;
constexpr mx_u32_t kStateRunning = 1u << 2;

struct RegistryEntry {
    char name[32];
    char image[32];
    char principal[24];
    char restart[16];
    mx_handle_t process;
    mx_handle_t main_thread;
    mx_u32_t state;
    mx_u32_t flags;
    mx_u64_t reserved[2];
};
static_assert(sizeof(RegistryEntry) == 144, "service registry entry must stay compact");

struct RegistryPage {
    mx_u32_t magic;
    mx_u16_t version;
    mx_u16_t entry_count;
    mx_u64_t generation;
    mx_u64_t reserved[6];
    RegistryEntry entries[kMaxRegistryEntries];
};
static_assert(sizeof(RegistryPage) <= 4096, "service registry must fit in one page");

inline mx_u64_t read_generation(const RegistryPage* page) {
    return __atomic_load_n(&page->generation, __ATOMIC_ACQUIRE);
}

inline mx_u64_t read_begin(const RegistryPage* page) {
    mx_u64_t gen = 0;
    do {
        gen = read_generation(page);
        if (gen & 1u) asm volatile("yield" ::: "memory");
    } while (gen & 1u);
    return gen;
}

inline bool read_retry(const RegistryPage* page, mx_u64_t begin_generation) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return (begin_generation & 1u) ||
           read_generation(page) != begin_generation;
}

inline void write_begin(RegistryPage* page) {
    mx_u64_t gen = __atomic_load_n(&page->generation, __ATOMIC_RELAXED);
    if ((gen & 1u) == 0) ++gen;
    __atomic_store_n(&page->generation, gen, __ATOMIC_RELEASE);
    // Publish odd before any following registry stores become visible.
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

inline void write_end(RegistryPage* page) {
    mx_u64_t gen = __atomic_load_n(&page->generation, __ATOMIC_RELAXED);
    if ((gen & 1u) == 0) ++gen;
    __atomic_store_n(&page->generation, gen + 1u, __ATOMIC_RELEASE);
}

} // namespace mochi::svc

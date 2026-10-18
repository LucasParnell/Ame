#pragma once
// Disk-resident keyed storage. RAM bounds limit transaction/cache working sets,
// never the number of files, extents, or addressable volume blocks.
#include "libk/bytes.h"
#include "midix/abi/protocols/fs.h"

namespace mochi::store {
using u8 = mx_u8_t;
using u16 = mx_u16_t;
using u32 = mx_u32_t;
using u64 = mx_u64_t;
constexpr u32 kBlock = 4096, kEntries = 63, kDepth = 8, kPages = 96, kBatch = 16, kFrees = 1024,
              kReserve = 32;
constexpr u32 kDataRoot = 0x80000000u;
struct Io {
    void *context;
    bool (*read)(void *, u64, u32, void *);
    bool (*write)(void *, u64, u32, const void *);
    bool (*flush)(void *);
};
struct State {
    Io io{};
    FsSuperblock super{};
    FsCheckpoint checkpoint{}, pending{};
    u64 allocation_hint{};
    u32 next_inode{}, page_count{}, free_count{}, orphans{}, reserve_count{};
    mx_i32_t error{};
    bool mounted{}, poisoned{}, transaction{}, reclaim{};
    // Hot page identities are separate from cold 4 KiB payloads.
    u64 page_ids[kPages]{}, freed[kFrees]{}, reserve[kReserve]{};
    alignas(4096) u8 pages[kPages][kBlock]{};
    alignas(4096) u8 path[kDepth][kBlock + 64]{};
    alignas(4096) u8 lookup[kBlock]{}, data[kBlock]{}, journal[kBlock]{};
    alignas(4096) u8 batch[kBatch][kBlock]{};
};
// mount replays only a completely validated committed journal. No automatic
// formatting, and any ambiguous I/O poisons writes until a fresh mount.
bool mount(State &, Io);
bool inode(State &, u32, InodeLeafEntry &);
mx_i32_t lookup(State &, u32, const char *, u8, InodeLeafEntry &);
mx_i32_t create(State &, u32, const char *, u8, u16, InodeLeafEntry &);
mx_i32_t read(State &, u32, u64, void *, u32, u32 &);
mx_i32_t write(State &, u32, u64, const void *, u32);
mx_i32_t truncate(State &, u32);
mx_i32_t unlink(State &, u32, const char *, u8);
mx_i32_t rename(State &, u32, const char *, u8, u32, const char *, u8);
mx_i32_t readdir(State &, u32, u32, DirLeafEntry &, bool &);
mx_i32_t sync(State &);
mx_i32_t promote(State &, u32, InodeLeafEntry &);
} // namespace mochi::store

#pragma once
// userland/fs/fs_internal.h — shared surface for the io.fs service modules.
//
// io.fs is a fixed-capacity, data-oriented filesystem service. The boot wiring
// and the per-client SPSC session loop live in fs_ring.cc; everything else is
// split by responsibility:
//
//   fs_core     — in-memory inode/extent/phys-ref core, checksums, formatting
//   fs_handles  — open-handle table, per-session handle ownership, handlers
//   fs_snapshot — copy-on-write snapshot pin/unpin and snapshot handlers
//   fs_disk     — the midix-fs-v1 disk backend (GPT, B+tree, block cache)
//   fs_ring     — boot wiring, per-client session loop, request dispatch
//
// Mount semantics, kept explicit here because the whole service depends on them
// (see also docs/FS_SPEC.md and docs/CLEANUP_STATUS.md item 5):
//
//   * The disk root is READ-ONLY. Every disk-backed handle is opened read-only
//     (disk handles carry no mutation access), and FS_OPEN_WRITE/CREATE/TRUNC
//     are rejected with MX_ERR_PERM before the request can reach the disk
//     B+tree. Redirecting/overlaying the root read-only means a client can never
//     mutate the signed disk image through io.fs.
//
//   * /tmp is VOLATILE. It lives only in the in-memory inode table, is
//     redirected away from the disk root on open (fs_handles handle_open), and
//     is discarded on service restart. Nothing under /tmp is ever written back
//     to the disk image.
//
//   * /data is the DURABLE user volume. It is also served from the in-memory
//     inode table (so the read-only root is never mutated), but on FS_SYNC the
//     whole /data subtree is serialised into the disk's reserved journal region
//     through the block service (BLK_WRITE) and flushed with BLK_FLUSH. On the
//     next mount the region is replayed, so a file written and synced survives a
//     reboot. When no writable region exists (memory backend, or a disk without
//     a usable reserved region) /data is an explicitly NON-DURABLE in-memory
//     volume and FS_SYNC returns MX_ERR_NOT_IMPLEMENTED. See fs_durable.cc.

#include "libk/bytes.h"
#include "libk/hash.h"
#include "libk/integer.h"
#include "libk/text.h"
#include "libk/str.h"
#include "mochi/va_layout.h"
#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/fs.h"
#include "mochi/boot.h"
#include "midix/abi/protocols/fs.h"
using namespace mochi::va::fs;

// ---------------------------------------------------------------------------
// Geometry and shared limits. All storage is fixed-capacity on purpose: the
// hot path never allocates, so every table is sized at compile time.
// ---------------------------------------------------------------------------
constexpr mx_u32_t kRingOrder = MX_DEFAULT_RING_ORDER;
constexpr mx_u32_t kSectorSize = 512;
constexpr mx_u32_t kBitsPerByte = 8;
constexpr mx_u32_t kRootInode = 1;
constexpr fs_handle_t kRootHandle = 1; // index 1 of g_handles / g_handle_generations
constexpr mx_u32_t kMaxInodes = 64;
constexpr mx_u32_t kMaxHandles = 64;
constexpr mx_u32_t kMaxExtentsPerInode = 8;
constexpr mx_u32_t kMaxPhysBlocks = 256;
constexpr mx_u32_t kMaxSnapshots = 8;
constexpr mx_u32_t kMaxName = 47;
constexpr mx_u32_t kNameBytes = kMaxName + 1; // inode name storage, NUL included
constexpr mx_u32_t kBlockSize = FS_BLOCK_SIZE;
constexpr mx_u32_t kSectorsPerFsBlock = kBlockSize / kSectorSize;
// Smallest reserved region io.fs will treat as a durable /data backing store.
constexpr mx_u64_t kMinDurableRegionBlocks = 8;

// Mount points resolved from the read-only root: /tmp is volatile, /data is the
// durable volume. Both are created in format_memory_fs() and recognised by
// handle_open() through these names.
constexpr char kTmpMountName[] = "tmp";
constexpr char kDataMountName[] = "data";
constexpr mx_u8_t kTmpMountNameLen = static_cast<mx_u8_t>(sizeof(kTmpMountName) - 1);
constexpr mx_u8_t kDataMountNameLen = static_cast<mx_u8_t>(sizeof(kDataMountName) - 1);

// Handle tokens pack a handle-table slot index and its generation:
// (generation << kHandleIndexBits) | index. Slots 0 and 1 are reserved (0 means
// "no handle", 1 is the read-only root), so client handles start at
// kFirstClientHandle. The generation is 24 bits wide; a slot that saturates it
// is retired rather than wrapped, so a token is never reused.
constexpr mx_u32_t kHandleIndexBits = 8;
constexpr mx_u32_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
constexpr mx_u32_t kFirstClientHandle = 2;
constexpr mx_u32_t kMaxHandleGeneration = (1u << 24) - 1;

struct ExtentState {
    bool used;
    mx_u64_t logical_block;
    mx_u64_t phys_block;
    mx_u64_t length_blocks;
    mx_u64_t generation;     // in-memory mirror of ExtentLeafEntry.generation
    mx_u32_t content_hash32; // FNV-1a tag over the backing data block
    mx_u16_t flags;          // in-memory mirror of ExtentLeafEntry.flags
};

struct InodeState {
    bool used;
    mx_u32_t inode_number;
    mx_u32_t parent_inode;
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u32_t nlink;
    mx_u64_t size;
    char name[kNameBytes];
    ExtentState extents[kMaxExtentsPerInode];
};

struct PhysRefState {
    bool used;
    mx_u64_t phys_block;
    mx_u64_t length_blocks;
    mx_u32_t refcount;
    mx_u32_t flags;
};

struct SnapshotState {
    bool used;
    mx_u64_t snapshot_id;
    mx_u64_t parent_snapshot_id;
    InodeState inodes[kMaxInodes];
};

struct OpenHandle {
    bool used;
    bool disk_backed;
    // Session ownership. owner is the directory slot index; owner_generation is
    // the slot generation at open time. Both must match the currently served
    // client for the handle to be accepted, so a token minted before a client
    // restart can never be replayed (see fs_handles handle_state).
    mx_u32_t owner;
    mx_u64_t owner_generation;
    mx_u32_t inode_number;
    mx_u64_t snapshot_id;
    mx_u32_t access; // FsAccess; checked by the operation handlers
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u64_t size;
    mx_u64_t tree_root;
};

// ---------------------------------------------------------------------------
// Shared tables (defined once, in fs_core.cc unless noted).
// ---------------------------------------------------------------------------
extern InodeState g_inodes[kMaxInodes];
extern PhysRefState g_phys_refs[kMaxPhysBlocks];
extern SnapshotState g_snapshots[kMaxSnapshots];
extern OpenHandle g_handles[kMaxHandles];
extern mx_u32_t g_handle_generations[kMaxHandles];
extern mx_u8_t g_data_blocks[kMaxPhysBlocks][kBlockSize];
extern mx_u32_t g_next_inode;
extern mx_u64_t g_generation;
extern mx_u64_t g_last_snapshot_id;
// Set by every metadata/data mutation, cleared once a checkpoint is committed
// (FS_SPEC §7.1 step 7). The ordered-update contract has no other clean/dirty
// boundary; a later metadata flush reads this before deciding to write back.
extern bool g_metadata_dirty;
// Inode number of the durable user volume root (/data); 0 if it could not be
// created. Set by fs_core format_memory_fs().
extern mx_u32_t g_data_inode;

// Currently served client, set by the session loop before dispatching a request.
extern mx_u32_t g_session;
extern mx_u64_t g_session_generation;

// ---------------------------------------------------------------------------
// Small freestanding helpers, shared by every module.
// ---------------------------------------------------------------------------
static inline mx_u32_t str_len(const char* s, mx_u32_t limit) {
    return static_cast<mx_u32_t>(libk::str::length_n(s, limit));
}

// Compare a terminated inode name to a counted protocol name.
static inline bool name_eq(const char* a, const char* b, mx_u8_t b_len) {
    return b_len <= kMaxName && a && b &&
           libk::str::length_n(a, b_len + 1) == b_len &&
           libk::str::equal_n(a, b, b_len);
}

static inline void copy_name(char (&dst)[kNameBytes], const char* src, mx_u8_t len) {
    libk::str::Buffer name(dst);
    name.append(src, len);
}

static inline mx_u32_t min_u32(mx_u32_t a, mx_u32_t b) {
    return a < b ? a : b;
}

// ---------------------------------------------------------------------------
// fs_core.cc — in-memory core.
//
// Two checksum families coexist on purpose; pick by where the value is checked:
//   * crc32_ieee / crc32_ieee_update — CRC-32/IEEE over blocks that leave the
//     service (superblock, GPT, durable records, disk extent tags).
//   * fnv1a32_block — FNV-1a over a block the service still owns, i.e. the
//     in-memory extent content tag.
// ---------------------------------------------------------------------------
mx_u32_t name_hash32(const char* name, mx_u8_t len);
mx_u32_t fnv1a32_block(const mx_u8_t* data);
// Record that metadata changed since the last committed checkpoint. The reason
// (alloc/free/inode/dir/refcount) is not tracked in V1; when the post-V1
// journal lands (FS_SPEC §7.2, JournalRecordType), the mutation site records
// the typed delta and this stays the cheap dirty bit.
void mark_metadata_dirty();

InodeState* live_inode(mx_u32_t inode_number);
InodeState* alloc_inode(mx_u16_t type, mx_u16_t inode_class, mx_u32_t parent_inode,
                        const char* name, mx_u8_t name_len);
const InodeState* find_child(mx_u64_t snapshot_id, mx_u32_t parent_inode, const char* name,
                             mx_u8_t name_len);
InodeState* find_live_child(mx_u32_t parent_inode, const char* name, mx_u8_t name_len);
const ExtentState* find_extent_const(const InodeState& inode, mx_u64_t logical_block);
mx_i32_t inc_phys_ref(mx_u64_t phys_block);
void dec_phys_ref(mx_u64_t phys_block);
void release_inode_extents(InodeState& inode);
mx_i32_t write_inline(InodeState& inode, mx_u64_t offset, const mx_u8_t* data, mx_u32_t len);
// Unbounded data write used by the durable replay path (write_inline caps at
// FS_INLINE_WRITE_MAX). Same CoW/checksum semantics as write_inline.
mx_i32_t write_bytes_unbounded(InodeState& inode, mx_u64_t offset, const mx_u8_t* data,
                               mx_u32_t len);
mx_i32_t verify_refcount_index();
void format_memory_fs();
void register_service_arenas();

// ---------------------------------------------------------------------------
// fs_handles.cc — handle table + request handlers.
// ---------------------------------------------------------------------------
fs_handle_t handle_token(mx_u32_t index);
fs_handle_t alloc_handle(mx_u32_t inode_number, mx_u64_t snapshot_id, bool read_only);
void close_session(mx_u32_t session);
void send_status_only(const mochi::fs::Server& fs, const RingSlot& req, mx_u32_t reply_type,
                      mx_i32_t status);

void handle_open(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_close(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_read(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_write(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_readdir(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_mkdir(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_unlink(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_rename(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_promote(const mochi::fs::Server& fs, const RingSlot& slot);

// ---------------------------------------------------------------------------
// fs_snapshot.cc — snapshot pin/unpin + handlers.
// ---------------------------------------------------------------------------
SnapshotState* snapshot_by_id(mx_u64_t snapshot_id);
const InodeState* any_inode(mx_u64_t snapshot_id, mx_u32_t inode_number);

void handle_snapshot(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_snapshot_mount(const mochi::fs::Server& fs, const RingSlot& slot);
void handle_snapshot_drop(const mochi::fs::Server& fs, const RingSlot& slot);

// ---------------------------------------------------------------------------
// fs_disk.cc — read-only midix-fs-v1 disk backend + durable region access.
// ---------------------------------------------------------------------------
bool disk_mounted();
// Read one fs block (cache-coherent) through the block service.
bool disk_read_block(mx_u64_t block, mx_u8_t* out);
// Write one fs block through the block service and keep the cache coherent.
bool disk_write_block(mx_u64_t block, const mx_u8_t* data);
// BLK_FLUSH; returns MX_OK when the device reports durable completion.
mx_i32_t disk_flush();
// The reserved, non-root region io.fs may use for durable user data. Returns
// false unless a mount succeeded and the region is provably disjoint from every
// block the read-only root allocates.
bool disk_durable_region(mx_u64_t* start_block, mx_u64_t* block_count);
bool disk_inode(mx_u32_t inode_number, InodeLeafEntry* out);
bool disk_dir_lookup(const InodeLeafEntry& dir, mx_u32_t parent_inode, const char* name,
                     mx_u8_t name_len, InodeLeafEntry* out_inode);
fs_handle_t alloc_disk_handle(const InodeLeafEntry& inode);
// ---------------------------------------------------------------------------
// fs_durable.cc — durable /data store backed by the reserved disk region.
// ---------------------------------------------------------------------------
bool durable_available();
// Replay the region into the in-memory /data subtree. Returns true when a
// committed region was fully validated and imported.
bool durable_load();
// Verify invariants, serialise /data into the region, and flush the device.
mx_i32_t durable_sync();
void handle_sync(const mochi::fs::Server& fs, const RingSlot& slot);
mx_i32_t disk_read_file(const OpenHandle& handle, mx_u64_t offset, mx_u32_t length,
                        FsReadReply* reply);
mx_i32_t disk_readdir(const OpenHandle& handle, const FsReaddirRequest& req, FsReaddirReply* reply);
void fs_disk_init(const MxBootInfo* boot);

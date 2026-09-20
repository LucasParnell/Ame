// userland/fs/main.cc — io.fs service.
//
// V1 bring-up implementation. This is a fixed-capacity filesystem core tuned
// for the hot path: ordered data-before-metadata updates, inode-owned CoW
// policy, extent checksums/generations, explicit physical refcounts, and the
// 64-byte ring ABI. Snapshot and replay hooks stay small until the durable
// B+tree writer lands behind the same operations.

#include "mochi/syscall.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/fs.h"
#include "mochi/sessions.h"
#include "midix/abi/protocols/fs.h"

constexpr mx_vaddr_t kVaBootInfo = 0x1000'0000ull;
constexpr mx_vaddr_t kVaLogRing = 0x1800'0000ull;
[[maybe_unused]] constexpr mx_vaddr_t kVaFsCmdRing = 0x2000'0000ull;
[[maybe_unused]] constexpr mx_vaddr_t kVaFsReplyRing = 0x2001'0000ull;
constexpr mx_vaddr_t kVaBlkReqRing = 0x2002'0000ull;
constexpr mx_vaddr_t kVaBlkReplyRing = 0x2003'0000ull;
constexpr mx_vaddr_t kVaBlkDma = 0x2100'0000ull;

constexpr mx_u32_t kRingOrder = MX_DEFAULT_RING_ORDER;
constexpr mx_u32_t kRootInode = 1;
constexpr fs_handle_t kRootHandle = 1;

constexpr mx_u32_t kMaxInodes = 64;
constexpr mx_u32_t kMaxHandles = 64;
constexpr mx_u32_t kMaxExtentsPerInode = 8;
constexpr mx_u32_t kMaxPhysBlocks = 256;
constexpr mx_u32_t kMaxSnapshots = 8;
constexpr mx_u32_t kMaxName = 47;
constexpr mx_u32_t kBlockSize = FS_BLOCK_SIZE;
constexpr mx_u32_t kSectorsPerFsBlock = FS_BLOCK_SIZE / 512;
constexpr mx_u16_t kBTreeInternal = 1;
constexpr mx_u16_t kBTreeLeaf = 2;
constexpr mx_u32_t kBTreeMaxEntries = 63;
constexpr mx_u32_t kDiskBlockCacheEntries = 32;

alignas(4096) static mx_u8_t g_metadata_arena[4096 * 2];
alignas(4096) static mx_u8_t g_inode_arena[4096 * 2];
alignas(4096) static mx_u8_t g_extent_arena[4096 * 2];
alignas(4096) static mx_u8_t g_snapshot_arena[4096 * 2];
alignas(4096) static mx_u8_t g_data_blocks[kMaxPhysBlocks][kBlockSize];
alignas(4096) static mx_u8_t g_disk_block[kBlockSize];

struct DiskBlockCacheEntry {
    bool valid;
    mx_u64_t block;
    mx_u64_t age;
    alignas(4096) mx_u8_t data[kBlockSize];
};

struct ExtentState {
    bool used;
    mx_u64_t logical_block;
    mx_u64_t phys_block;
    mx_u64_t length_blocks;
    mx_u64_t generation;
    mx_u32_t data_crc32;
    mx_u16_t flags;
};

struct InodeState {
    bool used;
    mx_u32_t inode_number;
    mx_u32_t parent_inode;
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u32_t mode;
    mx_u32_t nlink;
    mx_u64_t size;
    char name[48];
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
    mx_u32_t owner;
    mx_u32_t inode_number;
    mx_u64_t snapshot_id;
    bool read_only;
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u32_t mode;
    mx_u64_t size;
    mx_u64_t tree_root;
};

static InodeState g_inodes[kMaxInodes];
static PhysRefState g_phys_refs[kMaxPhysBlocks];
static SnapshotState g_snapshots[kMaxSnapshots];
static OpenHandle g_handles[kMaxHandles];
static mx_u32_t g_handle_generations[kMaxHandles];
static mx_u32_t g_session;
[[maybe_unused]] static mx_u32_t g_tmp_inode;
static mochi::ServerSessions g_sessions;

static fs_handle_t handle_token(mx_u32_t index) {
    g_handles[index].owner = g_session;
    return (++g_handle_generations[index] << 8) | index;
}
static void close_session(mx_u32_t session) {
    for (mx_u32_t i = 2; i < kMaxHandles; ++i)
        if (g_handles[i].used && g_handles[i].owner == session) g_handles[i] = {};
}

static mochi::fs::BlkClient g_blk_client{};
static bool g_blk_available = false;
static bool g_disk_mounted = false;
static mx_u8_t* g_blk_dma = nullptr;
static mx_u64_t g_blk_dma_size = 0;
static mx_u64_t g_disk_base_sector = 0;
static mx_u64_t g_disk_partition_sectors = 0;
static mx_u64_t g_disk_total_blocks = 0;
static mx_u64_t g_disk_inode_root = 0;
static DiskBlockCacheEntry g_disk_cache[kDiskBlockCacheEntries];
static mx_u64_t g_disk_cache_clock = 1;
alignas(4096) static mx_u8_t g_gpt_entries[64 * 1024];
static mx_u32_t g_next_inode = kRootInode + 1;
static mx_u64_t g_generation = 1;
static mx_u64_t g_last_snapshot_id = 0;
static bool g_metadata_dirty = false;

static const char kHelloName[] = "hello.txt";
static const char kHelloData[] =
    "Midix io.fs V1: ordered CoW extents and inline I/O are alive.\n";

static void zero_bytes(void* ptr, mx_u64_t len) {
    auto* p = static_cast<mx_u8_t*>(ptr);
    for (mx_u64_t i = 0; i < len; ++i) p[i] = 0;
}

static void fs_copy_bytes(void* dst, const void* src, mx_u64_t len) {
    auto* d = static_cast<mx_u8_t*>(dst);
    const auto* s = static_cast<const mx_u8_t*>(src);
    for (mx_u64_t i = 0; i < len; ++i) d[i] = s[i];
}

static mx_u32_t str_len(const char* s, mx_u32_t limit) {
    mx_u32_t n = 0;
    if (!s) return 0;
    while (n < limit && s[n]) ++n;
    return n;
}

static bool name_eq(const char* a, const char* b, mx_u8_t b_len) {
    for (mx_u32_t i = 0; i < b_len; ++i) {
        if (a[i] != b[i]) return false;
    }
    return a[b_len] == '\0';
}

static void copy_name(char* dst, const char* src, mx_u8_t len) {
    mx_u32_t n = len;
    if (n > kMaxName) n = kMaxName;
    for (mx_u32_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = '\0';
}

static mx_u32_t min_u32(mx_u32_t a, mx_u32_t b) {
    return a < b ? a : b;
}

static mx_u32_t crc32_ieee(const mx_u8_t* data, mx_u64_t len) {
    mx_u32_t crc = 0xFFFF'FFFFu;
    for (mx_u64_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (mx_u32_t bit = 0; bit < 8; ++bit) {
            const mx_u32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB8'8320u & mask);
        }
    }
    return ~crc;
}

static mx_u32_t name_hash32(const char* name, mx_u8_t len) {
    return crc32_ieee(reinterpret_cast<const mx_u8_t*>(name), len);
}

enum class FsMutation : mx_u32_t {
    ExtentAlloc = 1,
    ExtentFree = 2,
    InodeWrite = 3,
    DirInsert = 4,
    DirDelete = 5,
    PhysRefInc = 6,
    PhysRefDec = 7,
};

static mx_u32_t checksum_block(const mx_u8_t* data) {
    mx_u32_t hash = 2166136261u;
    for (mx_u32_t i = 0; i < kBlockSize; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void note_metadata_delta(FsMutation type, mx_u32_t target) {
    (void)type;
    (void)target;
    g_metadata_dirty = true;
}

static InodeState* live_inode(mx_u32_t inode_number) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (g_inodes[i].used && g_inodes[i].inode_number == inode_number) return &g_inodes[i];
    }
    return nullptr;
}

static SnapshotState* snapshot_by_id(mx_u64_t snapshot_id) {
    if (snapshot_id == 0) return nullptr;
    for (mx_u32_t i = 0; i < kMaxSnapshots; ++i) {
        if (g_snapshots[i].used && g_snapshots[i].snapshot_id == snapshot_id) {
            return &g_snapshots[i];
        }
    }
    return nullptr;
}

static const InodeState* snapshot_inode(mx_u64_t snapshot_id, mx_u32_t inode_number) {
    const SnapshotState* snap = snapshot_by_id(snapshot_id);
    if (!snap) return nullptr;
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (snap->inodes[i].used && snap->inodes[i].inode_number == inode_number) {
            return &snap->inodes[i];
        }
    }
    return nullptr;
}

static const InodeState* any_inode(mx_u64_t snapshot_id, mx_u32_t inode_number) {
    if (snapshot_id) return snapshot_inode(snapshot_id, inode_number);
    return live_inode(inode_number);
}

static InodeState* alloc_inode(mx_u16_t type, mx_u16_t inode_class,
                               mx_u32_t parent_inode, const char* name,
                               mx_u8_t name_len, mx_u32_t mode) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (g_inodes[i].used) continue;
        InodeState& inode = g_inodes[i];
        zero_bytes(&inode, sizeof(inode));
        inode.used = true;
        inode.inode_number = g_next_inode++;
        inode.parent_inode = parent_inode;
        inode.type = type;
        inode.inode_class = inode_class;
        inode.mode = mode;
        inode.nlink = 1;
        inode.size = 0;
        copy_name(inode.name, name, name_len);
        note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
        return &inode;
    }
    return nullptr;
}

static PhysRefState* phys_ref(mx_u64_t phys_block) {
    for (mx_u32_t i = 0; i < kMaxPhysBlocks; ++i) {
        if (g_phys_refs[i].used && g_phys_refs[i].phys_block == phys_block) {
            return &g_phys_refs[i];
        }
    }
    return nullptr;
}

static mx_i32_t inc_phys_ref(mx_u64_t phys_block) {
    PhysRefState* ref = phys_ref(phys_block);
    if (!ref) return static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
    if (ref->refcount == 0xFFFF'FFFFu) return static_cast<mx_i32_t>(MX_ERR_OVERFLOW);
    ++ref->refcount;
    note_metadata_delta(FsMutation::PhysRefInc, static_cast<mx_u32_t>(phys_block));
    return MX_OK;
}

static void dec_phys_ref(mx_u64_t phys_block) {
    PhysRefState* ref = phys_ref(phys_block);
    if (!ref || ref->refcount == 0) return;
    --ref->refcount;
    note_metadata_delta(FsMutation::PhysRefDec, static_cast<mx_u32_t>(phys_block));
    if (ref->refcount == 0) {
        zero_bytes(g_data_blocks[phys_block], kBlockSize);
        zero_bytes(ref, sizeof(*ref));
        note_metadata_delta(FsMutation::ExtentFree, static_cast<mx_u32_t>(phys_block));
    }
}

static mx_i32_t alloc_phys_block(mx_u64_t* out_phys) {
    for (mx_u32_t i = 1; i < kMaxPhysBlocks; ++i) {
        if (g_phys_refs[i].used) continue;
        g_phys_refs[i].used = true;
        g_phys_refs[i].phys_block = i;
        g_phys_refs[i].length_blocks = 1;
        g_phys_refs[i].refcount = 1;
        g_phys_refs[i].flags = 0;
        zero_bytes(g_data_blocks[i], kBlockSize);
        if (out_phys) *out_phys = i;
        note_metadata_delta(FsMutation::ExtentAlloc, i);
        return MX_OK;
    }
    return static_cast<mx_i32_t>(MX_ERR_NOMEM);
}

static ExtentState* find_extent(InodeState& inode, mx_u64_t logical_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        ExtentState& extent = inode.extents[i];
        if (!extent.used) continue;
        if (logical_block >= extent.logical_block &&
            logical_block < extent.logical_block + extent.length_blocks) {
            return &extent;
        }
    }
    return nullptr;
}

static const ExtentState* find_extent_const(const InodeState& inode, mx_u64_t logical_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        const ExtentState& extent = inode.extents[i];
        if (!extent.used) continue;
        if (logical_block >= extent.logical_block &&
            logical_block < extent.logical_block + extent.length_blocks) {
            return &extent;
        }
    }
    return nullptr;
}

static ExtentState* alloc_extent_slot(InodeState& inode, mx_u64_t logical_block,
                                      mx_u64_t phys_block) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        if (inode.extents[i].used) continue;
        ExtentState& extent = inode.extents[i];
        extent.used = true;
        extent.logical_block = logical_block;
        extent.phys_block = phys_block;
        extent.length_blocks = 1;
        extent.generation = g_generation;
        extent.data_crc32 = checksum_block(g_data_blocks[phys_block]);
        extent.flags = 0;
        note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
        return &extent;
    }
    return nullptr;
}

static mx_i32_t ensure_writable_block(InodeState& inode, mx_u64_t logical_block,
                                      mx_u64_t* out_phys) {
    ExtentState* extent = find_extent(inode, logical_block);
    if (!extent) {
        mx_u64_t phys = 0;
        const mx_i32_t rc = alloc_phys_block(&phys);
        if (rc != MX_OK) return rc;
        extent = alloc_extent_slot(inode, logical_block, phys);
        if (!extent) {
            dec_phys_ref(phys);
            return static_cast<mx_i32_t>(MX_ERR_NOMEM);
        }
        if (out_phys) *out_phys = phys;
        return MX_OK;
    }

    PhysRefState* ref = phys_ref(extent->phys_block);
    if (!ref) return static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
    if (ref->refcount > 1) {
        mx_u64_t new_phys = 0;
        const mx_i32_t rc = alloc_phys_block(&new_phys);
        if (rc != MX_OK) return rc;
        fs_copy_bytes(g_data_blocks[new_phys], g_data_blocks[extent->phys_block], kBlockSize);
        dec_phys_ref(extent->phys_block);
        extent->phys_block = new_phys;
        extent->generation = g_generation;
        extent->data_crc32 = checksum_block(g_data_blocks[new_phys]);
        note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
    }

    if (out_phys) *out_phys = extent->phys_block;
    return MX_OK;
}

static const InodeState* find_child(mx_u64_t snapshot_id, mx_u32_t parent_inode,
                                    const char* name, mx_u8_t name_len) {
    if (snapshot_id) {
        const SnapshotState* snap = snapshot_by_id(snapshot_id);
        if (!snap) return nullptr;
        for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
            const InodeState& inode = snap->inodes[i];
            if (inode.used && inode.parent_inode == parent_inode &&
                name_eq(inode.name, name, name_len)) {
                return &inode;
            }
        }
        return nullptr;
    }

    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const InodeState& inode = g_inodes[i];
        if (inode.used && inode.parent_inode == parent_inode &&
            name_eq(inode.name, name, name_len)) {
            return &inode;
        }
    }
    return nullptr;
}

static InodeState* find_live_child(mx_u32_t parent_inode, const char* name, mx_u8_t name_len) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        InodeState& inode = g_inodes[i];
        if (inode.used && inode.parent_inode == parent_inode &&
            name_eq(inode.name, name, name_len)) {
            return &inode;
        }
    }
    return nullptr;
}

static fs_handle_t alloc_handle(mx_u32_t inode_number, mx_u64_t snapshot_id, bool read_only) {
    for (mx_u32_t i = 2; i < kMaxHandles; ++i) {
        if (g_handles[i].used || g_handle_generations[i] == 0xffffff) continue;
        g_handles[i].used = true;
        g_handles[i].inode_number = inode_number;
        g_handles[i].snapshot_id = snapshot_id;
        g_handles[i].read_only = read_only || snapshot_id != 0;
        return handle_token(i);
    }
    return 0;
}

static OpenHandle* handle_state(fs_handle_t handle) {
    if (handle == kRootHandle) return &g_handles[kRootHandle];
    const mx_u32_t index = handle & 255;
    if (index < 2 || index >= kMaxHandles || !g_handles[index].used ||
        (handle >> 8) != g_handle_generations[index] || g_handles[index].owner != g_session) return nullptr;
    return &g_handles[index];
}

static mx_i32_t parent_from_token(mx_u32_t token, mx_u32_t* out_inode,
                                  mx_u64_t* out_snapshot, bool* out_read_only) {
    const auto* h = handle_state(token ? token : kRootHandle);
    if (!h) return MX_ERR_NOT_FOUND;
    if (h->disk_backed) return MX_ERR_PERM;
    const auto* inode = any_inode(h->snapshot_id, h->inode_number);
    if (!inode || inode->type != FS_INODE_DIR) return MX_ERR_BAD_TYPE;
    if (out_inode) *out_inode = h->inode_number;
    if (out_snapshot) *out_snapshot = h->snapshot_id;
    if (out_read_only) *out_read_only = h->read_only;
    return MX_OK;
}

static void release_inode_extents(InodeState& inode) {
    for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
        if (!inode.extents[i].used) continue;
        dec_phys_ref(inode.extents[i].phys_block);
        zero_bytes(&inode.extents[i], sizeof(inode.extents[i]));
    }
}

static mx_i32_t verify_extent_table(const InodeState* table, mx_u32_t count) {
    for (mx_u32_t i = 0; i < count; ++i) {
        if (!table[i].used) continue;
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            const ExtentState& extent = table[i].extents[e];
            if (!extent.used) continue;
            if (extent.phys_block >= kMaxPhysBlocks || !phys_ref(extent.phys_block)) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
            if (checksum_block(g_data_blocks[extent.phys_block]) != extent.data_crc32) {
                return static_cast<mx_i32_t>(MX_ERR_FAULT);
            }
        }
    }
    return MX_OK;
}

static mx_u32_t count_extent_refs(const InodeState* table, mx_u32_t count,
                                  mx_u64_t phys_block) {
    mx_u32_t refs = 0;
    for (mx_u32_t i = 0; i < count; ++i) {
        if (!table[i].used) continue;
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            const ExtentState& extent = table[i].extents[e];
            if (extent.used && extent.phys_block == phys_block) ++refs;
        }
    }
    return refs;
}

static mx_i32_t verify_refcount_index() {
    mx_i32_t rc = verify_extent_table(g_inodes, kMaxInodes);
    if (rc != MX_OK) return rc;
    for (mx_u32_t s = 0; s < kMaxSnapshots; ++s) {
        if (!g_snapshots[s].used) continue;
        rc = verify_extent_table(g_snapshots[s].inodes, kMaxInodes);
        if (rc != MX_OK) return rc;
    }

    // bcachefs-shaped V1 invariant: extents are the index, phys refs are a
    // derived authority, and sync checks that the two agree.
    for (mx_u32_t p = 0; p < kMaxPhysBlocks; ++p) {
        const PhysRefState& ref = g_phys_refs[p];
        if (!ref.used) continue;
        mx_u32_t counted = count_extent_refs(g_inodes, kMaxInodes, ref.phys_block);
        for (mx_u32_t s = 0; s < kMaxSnapshots; ++s) {
            if (g_snapshots[s].used) {
                counted += count_extent_refs(g_snapshots[s].inodes, kMaxInodes, ref.phys_block);
            }
        }
        if (counted != ref.refcount) return static_cast<mx_i32_t>(MX_ERR_FAULT);
    }
    return MX_OK;
}

static mx_i32_t flush_block_device_if_available() {
    if (!g_blk_available) return MX_OK;
    BlkReply reply{};
    if (!g_blk_client.request(BLK_FLUSH, 0, 0, MX_NULL_HANDLE, 0, &reply)) {
        return static_cast<mx_i32_t>(MX_ERR_BUSY);
    }
    if (reply.status == static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED)) return MX_OK;
    return reply.status;
}

[[maybe_unused]] static mx_i32_t sync_ordered_metadata() {
    const mx_i32_t rc = verify_refcount_index();
    if (rc != MX_OK) return rc;
    if (!g_metadata_dirty) return MX_OK;
    // ext4 data=ordered shape: data pages are made durable before metadata is
    // declared clean; V1 keeps the metadata log tiny and relies on ordering.
    const mx_i32_t flush = flush_block_device_if_available();
    if (flush != MX_OK) return flush;
    g_metadata_dirty = false;
    return MX_OK;
}

static mx_i32_t write_inline(InodeState& inode, mx_u64_t offset,
                             const mx_u8_t* data, mx_u32_t len) {
    if (inode.type != FS_INODE_FILE) return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    if (inode.inode_class == FS_CLASS_CONTENT_STORE && inode.size != 0) {
        return static_cast<mx_i32_t>(MX_ERR_PERM);
    }
    if (len > FS_INLINE_WRITE_MAX) return static_cast<mx_i32_t>(MX_ERR_INVALID);

    mx_u32_t written = 0;
    while (written < len) {
        const mx_u64_t abs = offset + written;
        const mx_u64_t logical_block = abs / kBlockSize;
        const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
        const mx_u32_t chunk = min_u32(len - written, kBlockSize - block_off);
        mx_u64_t phys = 0;
        const mx_i32_t rc = ensure_writable_block(inode, logical_block, &phys);
        if (rc != MX_OK) return rc;
        fs_copy_bytes(g_data_blocks[phys] + block_off, data + written, chunk);
        ExtentState* extent = find_extent(inode, logical_block);
        if (!extent) return static_cast<mx_i32_t>(MX_ERR_FAULT);
        extent->data_crc32 = checksum_block(g_data_blocks[phys]);
        written += chunk;
    }

    if (offset + len > inode.size) inode.size = offset + len;
    ++g_generation;
    note_metadata_delta(FsMutation::InodeWrite, inode.inode_number);
    return MX_OK;
}

static void init_file_content(InodeState& inode, const char* data, mx_u32_t len) {
    inode.size = 0;
    inode.inode_class = FS_CLASS_CONTENT_STORE;
    const mx_u32_t old_class = inode.inode_class;
    inode.inode_class = FS_CLASS_USER_DATA;
    (void)write_inline(inode, 0, reinterpret_cast<const mx_u8_t*>(data), min_u32(len, FS_INLINE_WRITE_MAX));
    if (len > FS_INLINE_WRITE_MAX) {
        mx_u32_t off = FS_INLINE_WRITE_MAX;
        while (off < len) {
            const mx_u32_t chunk = min_u32(FS_INLINE_WRITE_MAX, len - off);
            (void)write_inline(inode, off, reinterpret_cast<const mx_u8_t*>(data + off), chunk);
            off += chunk;
        }
    }
    inode.inode_class = static_cast<mx_u16_t>(old_class);
}

static void format_memory_fs() {
    zero_bytes(g_inodes, sizeof(g_inodes));
    zero_bytes(g_phys_refs, sizeof(g_phys_refs));
    zero_bytes(g_snapshots, sizeof(g_snapshots));
    zero_bytes(g_handles, sizeof(g_handles));
    zero_bytes(g_data_blocks, sizeof(g_data_blocks));
    g_next_inode = kRootInode + 1;
    g_generation = 1;
    g_last_snapshot_id = 0;
    g_metadata_dirty = false;

    InodeState& root = g_inodes[0];
    root.used = true;
    root.inode_number = kRootInode;
    root.parent_inode = 0;
    root.type = FS_INODE_DIR;
    root.inode_class = FS_CLASS_USER_DATA;
    root.mode = 0755;
    root.nlink = 1;
    copy_name(root.name, "", 0);

    g_handles[kRootHandle].used = true;
    g_handles[kRootHandle].inode_number = kRootInode;
    g_handles[kRootHandle].snapshot_id = 0;
    g_handles[kRootHandle].read_only = true;
    g_handles[kRootHandle].type = FS_INODE_DIR;
    auto* tmp = alloc_inode(FS_INODE_DIR, FS_CLASS_USER_DATA, kRootInode, "tmp", 3, 0755);
    g_tmp_inode = tmp ? tmp->inode_number : 0;

    InodeState* hello = alloc_inode(FS_INODE_FILE, FS_CLASS_CONTENT_STORE,
                                    kRootInode, kHelloName,
                                    static_cast<mx_u8_t>(str_len(kHelloName, sizeof(kHelloName))),
                                    0444);
    if (hello) {
        init_file_content(*hello, kHelloData, static_cast<mx_u32_t>(sizeof(kHelloData) - 1));
    }
}

static RingPage* map_ring_from_boot(const MxBootInfo* boot, const char* name,
                                    mx_vaddr_t va, mx_u32_t* out_order) {
    const MxBootEntry* entry = mochi::boot::find_type(boot, name, MX_BOOT_ENTRY_RING);
    if (!entry || entry->handle == MX_NULL_HANDLE) return nullptr;
    if (mx_map_page(entry->handle, va, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
        return nullptr;
    }
    if (out_order) {
        *out_order = entry->aux0 ? static_cast<mx_u32_t>(entry->aux0) : kRingOrder;
    }
    return reinterpret_cast<RingPage*>(va);
}

static void register_service_arenas() {
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_metadata_arena),
                             sizeof(g_metadata_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_inode_arena),
                             sizeof(g_inode_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_extent_arena),
                             sizeof(g_extent_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_snapshot_arena),
                             sizeof(g_snapshot_arena), 0);
    (void)sys_arena_register(reinterpret_cast<mx_vaddr_t>(g_data_blocks),
                             sizeof(g_data_blocks), 0);
}

static mx_u32_t load_le32(const mx_u8_t* p) {
    return static_cast<mx_u32_t>(p[0]) |
           (static_cast<mx_u32_t>(p[1]) << 8) |
           (static_cast<mx_u32_t>(p[2]) << 16) |
           (static_cast<mx_u32_t>(p[3]) << 24);
}

static mx_u64_t load_le64(const mx_u8_t* p) {
    mx_u64_t v = 0;
    for (mx_u32_t i = 0; i < 8; ++i) v |= static_cast<mx_u64_t>(p[i]) << (i * 8);
    return v;
}

static bool disk_cache_lookup(mx_u64_t block, mx_u8_t* out) {
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        DiskBlockCacheEntry& entry = g_disk_cache[i];
        if (!entry.valid || entry.block != block) continue;
        entry.age = ++g_disk_cache_clock;
        fs_copy_bytes(out, entry.data, kBlockSize);
        return true;
    }
    return false;
}

static DiskBlockCacheEntry& disk_cache_victim() {
    mx_u32_t victim = 0;
    mx_u64_t oldest = ~0ull;
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        if (!g_disk_cache[i].valid) return g_disk_cache[i];
        if (g_disk_cache[i].age < oldest) {
            oldest = g_disk_cache[i].age;
            victim = i;
        }
    }
    return g_disk_cache[victim];
}

static void disk_cache_store(mx_u64_t block, const mx_u8_t* data) {
    DiskBlockCacheEntry& entry = disk_cache_victim();
    entry.valid = true;
    entry.block = block;
    entry.age = ++g_disk_cache_clock;
    fs_copy_bytes(entry.data, data, kBlockSize);
}

static void disk_cache_reset() {
    for (mx_u32_t i = 0; i < kDiskBlockCacheEntries; ++i) {
        g_disk_cache[i].valid = false;
        g_disk_cache[i].block = 0;
        g_disk_cache[i].age = 0;
    }
    g_disk_cache_clock = 1;
}

static bool disk_read_sectors(mx_u64_t sector, mx_u32_t sector_count, mx_u8_t* out) {
    if (!g_blk_available || !g_blk_dma || !out || sector_count == 0) return false;
    const mx_u64_t byte_count = static_cast<mx_u64_t>(sector_count) * 512ull;
    if (g_blk_dma_size < BLK_DMA_CLIENT_OFFSET ||
        byte_count > g_blk_dma_size - BLK_DMA_CLIENT_OFFSET) {
        return false;
    }
    BlkReply reply{};
    if (!g_blk_client.request(BLK_READ,
                              sector,
                              sector_count,
                              MX_NULL_HANDLE,
                              BLK_DMA_CLIENT_OFFSET,
                              &reply)) {
        return false;
    }
    if (reply.status != MX_OK || reply.sectors_transferred != sector_count) return false;
    fs_copy_bytes(out, g_blk_dma + BLK_DMA_CLIENT_OFFSET, byte_count);
    return true;
}

static bool disk_read_block(mx_u64_t block, mx_u8_t* out) {
    if (!g_blk_available || !g_blk_dma || !out) return false;
    if (g_disk_total_blocks != 0 && block >= g_disk_total_blocks) return false;
    if (disk_cache_lookup(block, out)) return true;
    const mx_u64_t sector = g_disk_base_sector + block * kSectorsPerFsBlock;
    if (g_disk_partition_sectors != 0 &&
        (sector < g_disk_base_sector ||
         sector + kSectorsPerFsBlock > g_disk_base_sector + g_disk_partition_sectors)) {
        return false;
    }
    if (!disk_read_sectors(sector, kSectorsPerFsBlock, out)) return false;
    disk_cache_store(block, out);
    return true;
}

enum class DiskTree : mx_u32_t {
    Inode,
    Extent,
    Directory,
};

static mx_u64_t disk_entry_key(const mx_u8_t* entry, DiskTree tree) {
    switch (tree) {
    case DiskTree::Inode:
        return load_le32(entry);
    case DiskTree::Extent:
    case DiskTree::Directory:
        return load_le64(entry);
    }
    return 0;
}

static bool disk_btree_find(mx_u64_t root, mx_u64_t key, DiskTree tree,
                            mx_u8_t out_entry[64]) {
    mx_u64_t block = root;
    for (mx_u32_t depth = 0; depth < 8; ++depth) {
        if (block == 0 || !disk_read_block(block, g_disk_block)) return false;
        const auto* header = reinterpret_cast<const BTreeNodeHeader*>(g_disk_block);
        if (header->entry_count > kBTreeMaxEntries) return false;
        if (header->node_type == kBTreeInternal) {
            mx_u64_t child = 0;
            const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
            for (mx_u32_t i = 0; i < header->entry_count; ++i) {
                const mx_u8_t* entry = entries + (i * 64);
                const mx_u64_t max_key = load_le64(entry);
                if (key <= max_key) {
                    child = load_le64(entry + 8);
                    break;
                }
            }
            if (child == 0) return false;
            block = child;
            continue;
        }
        if (header->node_type != kBTreeLeaf) return false;

        const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
        for (mx_u32_t i = 0; i < header->entry_count; ++i) {
            const mx_u8_t* entry = entries + (i * 64);
            const mx_u64_t entry_key = disk_entry_key(entry, tree);
            if (entry_key == key) {
                fs_copy_bytes(out_entry, entry, 64);
                return true;
            }
            if (entry_key > key) return false;
        }
        return false;
    }
    return false;
}

static bool disk_inode(mx_u32_t inode_number, InodeLeafEntry* out) {
    if (!g_disk_mounted || !out) return false;
    alignas(8) mx_u8_t entry[64];
    if (!disk_btree_find(g_disk_inode_root, inode_number, DiskTree::Inode, entry)) return false;
    fs_copy_bytes(out, entry, sizeof(InodeLeafEntry));
    return out->inode_number == inode_number;
}

static bool disk_dir_lookup(const InodeLeafEntry& dir, mx_u32_t parent_inode,
                            const char* name, mx_u8_t name_len,
                            InodeLeafEntry* out_inode) {
    if (dir.type != FS_INODE_DIR || name_len == 0 || !out_inode) return false;
    alignas(8) mx_u8_t entry[64];
    const mx_u64_t key =
        (static_cast<mx_u64_t>(parent_inode) << 32) | name_hash32(name, name_len);
    if (!disk_btree_find(dir.extent_tree_root, key, DiskTree::Directory, entry)) return false;
    const auto* dirent = reinterpret_cast<const DirLeafEntry*>(entry);
    if (dirent->compound_key != key || dirent->name_len != name_len) return false;
    for (mx_u32_t i = 0; i < name_len; ++i) {
        if (dirent->name[i] != name[i]) return false;
    }
    return disk_inode(dirent->child_inode, out_inode);
}

static bool disk_btree_nth_entry(mx_u64_t root, mx_u32_t target,
                                 mx_u32_t* cursor, mx_u8_t out_entry[64]) {
    if (root == 0 || !cursor || !out_entry) return false;
    if (!disk_read_block(root, g_disk_block)) return false;
    const auto* header = reinterpret_cast<const BTreeNodeHeader*>(g_disk_block);
    if (header->entry_count > kBTreeMaxEntries) return false;
    const mx_u8_t* entries = g_disk_block + sizeof(BTreeNodeHeader);
    if (header->node_type == kBTreeLeaf) {
        for (mx_u32_t i = 0; i < header->entry_count; ++i) {
            if (*cursor == target) {
                fs_copy_bytes(out_entry, entries + (i * 64), 64);
                return true;
            }
            ++(*cursor);
        }
        return false;
    }
    if (header->node_type != kBTreeInternal) return false;

    mx_u64_t children[kBTreeMaxEntries];
    const mx_u32_t count = header->entry_count;
    for (mx_u32_t i = 0; i < count; ++i) {
        children[i] = load_le64(entries + (i * 64) + 8);
    }
    for (mx_u32_t i = 0; i < count; ++i) {
        if (disk_btree_nth_entry(children[i], target, cursor, out_entry)) return true;
    }
    return false;
}

static mx_i32_t disk_read_file(const OpenHandle& handle, mx_u64_t offset,
                               mx_u32_t length, FsReadReply* reply) {
    if (!reply) return static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (handle.type != FS_INODE_FILE) return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    if (length > FS_INLINE_READ_MAX) return static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED);
    if (offset >= handle.size) {
        reply->length_read = 0;
        return MX_OK;
    }
    const mx_u32_t remaining = static_cast<mx_u32_t>(handle.size - offset);
    const mx_u32_t n = min_u32(length, min_u32(remaining, FS_INLINE_READ_MAX));
    mx_u32_t copied = 0;
    while (copied < n) {
        const mx_u64_t abs = offset + copied;
        const mx_u64_t logical_block = abs / kBlockSize;
        const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
        const mx_u32_t chunk = min_u32(n - copied, kBlockSize - block_off);
        alignas(8) mx_u8_t entry[64];
        if (!disk_btree_find(handle.tree_root, logical_block, DiskTree::Extent, entry)) {
            zero_bytes(reply->inline_data + copied, chunk);
            copied += chunk;
            continue;
        }
        const auto* extent = reinterpret_cast<const ExtentLeafEntry*>(entry);
        if (extent->logical_block != logical_block ||
            extent->phys_block >= g_disk_total_blocks ||
            !disk_read_block(extent->phys_block, g_disk_block)) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        if (extent->data_crc32 != 0 &&
            crc32_ieee(g_disk_block, kBlockSize) != extent->data_crc32) {
            return static_cast<mx_i32_t>(MX_ERR_FAULT);
        }
        fs_copy_bytes(reply->inline_data + copied, g_disk_block + block_off, chunk);
        copied += chunk;
    }
    reply->length_read = n;
    return MX_OK;
}

static mx_i32_t disk_readdir(const OpenHandle& handle,
                             const FsReaddirRequest& req,
                             FsReaddirReply* reply) {
    if (!reply) return static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (handle.type != FS_INODE_DIR) return static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
    mx_u32_t cursor = 0;
    alignas(8) mx_u8_t entry[64];
    if (!disk_btree_nth_entry(handle.tree_root, req.cookie, &cursor, entry)) {
        reply->flags = FS_READDIR_EOF;
        reply->entry_count = cursor;
        return MX_OK;
    }
    const auto* dirent = reinterpret_cast<const DirLeafEntry*>(entry);
    reply->flags = FS_READDIR_HAS_ENTRY;
    reply->next_cookie = req.cookie + 1;
    reply->inode_number = dirent->child_inode;
    reply->type = dirent->file_type;
    mx_u32_t n = dirent->name_len;
    if (n > sizeof(reply->name)) {
        n = sizeof(reply->name);
        reply->flags |= FS_READDIR_TRUNCATED;
    }
    reply->name_len = static_cast<mx_u8_t>(n);
    for (mx_u32_t i = 0; i < n; ++i) reply->name[i] = dirent->name[i];
    mx_u32_t after = 0;
    mx_u8_t ignored[64];
    if (!disk_btree_nth_entry(handle.tree_root, req.cookie + 1, &after, ignored)) {
        reply->flags |= FS_READDIR_EOF;
    }
    reply->entry_count = after;
    return MX_OK;
}

static fs_handle_t alloc_disk_handle(const InodeLeafEntry& inode) {
    for (mx_u32_t i = 2; i < kMaxHandles; ++i) {
        if (g_handles[i].used || g_handle_generations[i] == 0xffffff) continue;
        OpenHandle& h = g_handles[i];
        zero_bytes(&h, sizeof(h));
        h.used = true;
        h.disk_backed = true;
        h.inode_number = inode.inode_number;
        h.read_only = true;
        h.type = inode.type;
        h.inode_class = inode.inode_class;
        h.mode = inode.mode;
        h.size = inode.size;
        h.tree_root = inode.extent_tree_root;
        return handle_token(i);
    }
    return 0;
}

static bool gpt_name_eq(const mx_u8_t* utf16le, const char* ascii) {
    mx_u32_t i = 0;
    for (; ascii[i]; ++i) {
        if (i * 2 + 1 >= 72) return false;
        if (utf16le[i * 2] != static_cast<mx_u8_t>(ascii[i]) ||
            utf16le[i * 2 + 1] != 0) {
            return false;
        }
    }
    if (i * 2 + 1 >= 72) return true;
    return utf16le[i * 2] == 0 && utf16le[i * 2 + 1] == 0;
}

static bool select_ameroot_partition() {
    g_disk_base_sector = 0;
    g_disk_partition_sectors = 0;
    if (!disk_read_sectors(1, 1, g_disk_block)) return false;
    if (g_disk_block[0] != 'E' || g_disk_block[1] != 'F' ||
        g_disk_block[2] != 'I' || g_disk_block[3] != ' ' ||
        g_disk_block[4] != 'P' || g_disk_block[5] != 'A' ||
        g_disk_block[6] != 'R' || g_disk_block[7] != 'T') {
        return false;
    }

    const mx_u32_t header_size = load_le32(g_disk_block + 12);
    const mx_u32_t header_crc = load_le32(g_disk_block + 16);
    if (header_size < 92 || header_size > 512) return false;
    g_disk_block[16] = g_disk_block[17] = g_disk_block[18] = g_disk_block[19] = 0;
    if (crc32_ieee(g_disk_block, header_size) != header_crc) return false;

    const mx_u64_t entries_lba = load_le64(g_disk_block + 72);
    const mx_u32_t entry_count = load_le32(g_disk_block + 80);
    const mx_u32_t entry_size = load_le32(g_disk_block + 84);
    const mx_u32_t entries_crc = load_le32(g_disk_block + 88);
    if (entry_count == 0 || entry_size < 128 || entry_size > 512) return false;
    const mx_u64_t entries_bytes64 = static_cast<mx_u64_t>(entry_count) * entry_size;
    if (entries_bytes64 == 0 || entries_bytes64 > sizeof(g_gpt_entries)) return false;
    const mx_u32_t entries_bytes = static_cast<mx_u32_t>(entries_bytes64);
    const mx_u32_t entries_sectors =
        static_cast<mx_u32_t>((entries_bytes64 + 511ull) / 512ull);
    if (!disk_read_sectors(entries_lba, entries_sectors, g_gpt_entries)) return false;
    if (crc32_ieee(g_gpt_entries, entries_bytes) != entries_crc) return false;

    for (mx_u32_t i = 0; i < entry_count; ++i) {
        const mx_u8_t* entry = g_gpt_entries + static_cast<mx_u64_t>(i) * entry_size;
        bool empty = true;
        for (mx_u32_t b = 0; b < 16; ++b) empty = empty && entry[b] == 0;
        if (empty) continue;
        if (!gpt_name_eq(entry + 56, "AMEROOT")) continue;
        const mx_u64_t first = load_le64(entry + 32);
        const mx_u64_t last = load_le64(entry + 40);
        if (first == 0 || last < first) return false;
        g_disk_base_sector = first;
        g_disk_partition_sectors = last - first + 1;
        return true;
    }
    return false;
}

static bool mount_disk_fs() {
    if (!g_blk_available || !g_blk_dma) return false;
    disk_cache_reset();
    (void)select_ameroot_partition();
    if (!disk_read_block(0, g_disk_block)) return false;
    const auto* sb = reinterpret_cast<const FsSuperblock*>(g_disk_block);
    if (sb->magic != FS_MAGIC ||
        sb->version != FS_SUPERBLOCK_VERSION ||
        sb->block_size != FS_BLOCK_SIZE ||
        sb->inode_table_root == 0 ||
        sb->total_blocks == 0) {
        return false;
    }
    g_disk_total_blocks = sb->total_blocks;
    if (g_disk_partition_sectors != 0 &&
        g_disk_total_blocks > g_disk_partition_sectors / kSectorsPerFsBlock) {
        return false;
    }
    g_disk_inode_root = sb->inode_table_root;
    g_disk_mounted = true;
    InodeLeafEntry root;
    zero_bytes(&root, sizeof(root));
    if (!disk_inode(kRootInode, &root) || root.type != FS_INODE_DIR) {
        g_disk_mounted = false;
        return false;
    }
    mochi::log_raw_marker("io.fs: mounted midix-fs-v1 disk\n");
    return true;
}

static void send_status_only(const mochi::fs::Server& fs, const RingSlot& req,
                             mx_u32_t reply_type, mx_i32_t status) {
    (void)fs.send_payload(req, reply_type, &status, sizeof(status));
}

static void handle_open(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsOpenReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);

    if (slot.length >= sizeof(FsOpenRequest)) {
        const auto& req = *reinterpret_cast<const FsOpenRequest*>(slot.payload);
        // Bound name_len once at the entry point. find_child() and
        // disk_dir_lookup() both iterate name[0..name_len) without bounding,
        // and the request lives inside the 48-byte ring slot on this thread's
        // stack — an unbounded name_len would let any client read past the
        // slot into adjacent stack memory. kMaxName matches the on-disk
        // dirent name field and the existing CREATE-path check.
        if (req.name_len > sizeof(req.name)) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);
            (void)fs.send_reply(slot, FS_OPEN_REPLY, reply);
            return;
        }
        const auto* parent_handle = handle_state(req.parent_inode ? req.parent_inode : kRootHandle);
        const bool root_parent = parent_handle && parent_handle->inode_number == kRootInode;
        const bool tmp_mount = root_parent && req.name_len == 3 && name_eq("tmp", req.name, 3);
        if (g_disk_mounted && !tmp_mount && parent_handle && (root_parent || parent_handle->disk_backed)) {
            bool disk_parent = false;
            InodeLeafEntry parent{};
            if (root_parent) disk_parent = disk_inode(kRootInode, &parent);
            else {
                const auto& h = *parent_handle;
                parent.inode_number = h.inode_number;
                parent.type = h.type;
                parent.inode_class = h.inode_class;
                parent.size = h.size;
                parent.extent_tree_root = h.tree_root;
                parent.mode = h.mode;
                disk_parent = h.type == FS_INODE_DIR;
            }
            if (req.flags & (FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNC)) {
                reply.status = MX_ERR_PERM;
                (void)fs.send_reply(slot, FS_OPEN_REPLY, reply);
                return;
            }
            if (disk_parent) {
                InodeLeafEntry target;
                zero_bytes(&target, sizeof(target));
                bool found = false;
                if (req.name_len == 0 && (req.parent_inode == 0 || req.parent_inode == kRootHandle)) {
                    target = parent;
                    found = true;
                } else {
                    found = disk_dir_lookup(parent,
                                            parent.inode_number,
                                            req.name,
                                            req.name_len,
                                            &target);
                }

                if (found) {
                    const fs_handle_t h = alloc_disk_handle(target);
                    if (!h) {
                        reply.status = static_cast<mx_i32_t>(MX_ERR_NOMEM);
                    } else {
                        reply.status = MX_OK;
                        reply.handle = h;
                        reply.inode_number = target.inode_number;
                        reply.size = target.size;
                        reply.type = target.type;
                        reply.inode_class = target.inode_class;
                        reply.mode = target.mode;
                    }
                }
                (void)fs.send_reply(slot, FS_OPEN_REPLY, reply);
                return;
            }
        }

        mx_u32_t parent_inode = kRootInode;
        mx_u64_t snapshot_id = 0;
        bool read_only_parent = false;
        if (tmp_mount) { parent_inode = kRootInode; read_only_parent = false; reply.status = MX_OK; }
        else reply.status = parent_from_token(req.parent_inode, &parent_inode,
                                         &snapshot_id, &read_only_parent);
        if (reply.status == MX_OK) {
            const InodeState* target = nullptr;
            if (req.name_len == 0 && (req.parent_inode == 0 || req.parent_inode == kRootHandle)) {
                target = any_inode(snapshot_id, kRootInode);
            } else {
                target = find_child(snapshot_id, parent_inode, req.name, req.name_len);
            }

            if (!target && snapshot_id == 0 && !read_only_parent &&
                (req.flags & FS_OPEN_CREATE) && req.name_len <= kMaxName) {
                target = alloc_inode(FS_INODE_FILE, FS_CLASS_USER_DATA,
                                     parent_inode, req.name, req.name_len, 0644);
            }

            if (target) {
                if ((req.flags & (FS_OPEN_WRITE | FS_OPEN_TRUNC)) &&
                    (snapshot_id || read_only_parent || target->inode_class == FS_CLASS_CONTENT_STORE)) {
                    reply.status = MX_ERR_PERM;
                    (void)fs.send_reply(slot, FS_OPEN_REPLY, reply);
                    return;
                }
                if ((req.flags & FS_OPEN_TRUNC) && snapshot_id == 0 && !read_only_parent) {
                    InodeState* live = live_inode(target->inode_number);
                    if (live && live->type == FS_INODE_FILE) {
                        release_inode_extents(*live);
                        live->size = 0;
                        live->inode_class = FS_CLASS_USER_DATA;
                    }
                    target = live;
                }
                const bool write_requested = (req.flags & FS_OPEN_WRITE) != 0;
                if (write_requested && (snapshot_id != 0 || read_only_parent ||
                                        target->inode_class == FS_CLASS_CONTENT_STORE)) {
                    reply.status = static_cast<mx_i32_t>(MX_ERR_PERM);
                } else {
                    const fs_handle_t h =
                        alloc_handle(target->inode_number, snapshot_id,
                                     snapshot_id != 0 || target->inode_number == kRootInode ||
                                     (target->type != FS_INODE_DIR && !(req.flags & FS_OPEN_WRITE)));
                    if (!h) {
                        reply.status = static_cast<mx_i32_t>(MX_ERR_NOMEM);
                    } else {
                        reply.status = MX_OK;
                        reply.handle = h;
                        reply.inode_number = target->inode_number;
                        reply.size = target->size;
                        reply.type = target->type;
                        reply.inode_class = target->inode_class;
                        reply.mode = target->mode;
                    }
                }
            } else if (reply.status == MX_OK) {
                reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
            }
        }
    }

    (void)fs.send_reply(slot, FS_OPEN_REPLY, reply);
}

static void handle_read(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsReadReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);

    if (slot.length >= sizeof(FsReadRequest)) {
        const auto& req = *reinterpret_cast<const FsReadRequest*>(slot.payload);
        const OpenHandle* handle = handle_state(req.handle);
        if (handle && handle->disk_backed) {
            reply.status = disk_read_file(*handle, req.offset, req.length, &reply);
            (void)fs.send_reply(slot, FS_READ_REPLY, reply);
            return;
        }
        const InodeState* inode = handle ? any_inode(handle->snapshot_id, handle->inode_number) : nullptr;
        if (!inode) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        } else if (inode->type != FS_INODE_FILE) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
        } else if (req.length > FS_INLINE_READ_MAX) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED);
        } else if (req.offset >= inode->size) {
            reply.status = MX_OK;
            reply.length_read = 0;
        } else {
            const mx_u32_t remaining = static_cast<mx_u32_t>(inode->size - req.offset);
            const mx_u32_t n = min_u32(req.length, min_u32(remaining, FS_INLINE_READ_MAX));
            mx_u32_t copied = 0;
            while (copied < n) {
                const mx_u64_t abs = req.offset + copied;
                const mx_u64_t logical_block = abs / kBlockSize;
                const mx_u32_t block_off = static_cast<mx_u32_t>(abs % kBlockSize);
                const mx_u32_t chunk = min_u32(n - copied, kBlockSize - block_off);
                const ExtentState* extent = find_extent_const(*inode, logical_block);
                if (extent) {
                    if (checksum_block(g_data_blocks[extent->phys_block]) != extent->data_crc32) {
                        reply.status = static_cast<mx_i32_t>(MX_ERR_FAULT);
                        break;
                    }
                    fs_copy_bytes(reply.inline_data + copied,
                                  g_data_blocks[extent->phys_block] + block_off,
                                  chunk);
                } else {
                    zero_bytes(reply.inline_data + copied, chunk);
                }
                copied += chunk;
            }
            if (reply.status != static_cast<mx_i32_t>(MX_ERR_FAULT)) {
                reply.status = MX_OK;
                reply.length_read = n;
            }
        }
    }

    (void)fs.send_reply(slot, FS_READ_REPLY, reply);
}

static void handle_write(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsWriteReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    reply.bytes_written = 0;

    if (slot.length >= sizeof(FsWriteRequest)) {
        const auto& req = *reinterpret_cast<const FsWriteRequest*>(slot.payload);
        OpenHandle* handle = handle_state(req.handle);
        InodeState* inode = (handle && !handle->disk_backed) ? live_inode(handle->inode_number) : nullptr;
        if (handle && handle->disk_backed) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_PERM);
        } else if (!handle || !inode) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        } else if (handle->read_only) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_PERM);
        } else if (req.length > FS_INLINE_WRITE_MAX) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);
        } else {
            reply.status = write_inline(*inode, req.offset, req.payload, req.length);
            if (reply.status == MX_OK) reply.bytes_written = req.length;
        }
    }

    (void)fs.send_reply(slot, FS_WRITE_REPLY, reply);
}

static void handle_readdir(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsReaddirReply reply{};
    reply.status = MX_OK;
    reply.entry_count = 0;
    reply.next_cookie = 0;
    reply.flags = FS_READDIR_EOF;

    if (slot.length >= sizeof(FsReaddirRequest)) {
        const auto& req = *reinterpret_cast<const FsReaddirRequest*>(slot.payload);
        const OpenHandle* handle = handle_state(req.handle);
        if (handle && handle->disk_backed) {
            reply.status = disk_readdir(*handle, req, &reply);
            (void)fs.send_reply(slot, FS_READDIR_REPLY, reply);
            return;
        }
        const InodeState* dir = handle ? any_inode(handle->snapshot_id, handle->inode_number) : nullptr;
        if (!dir) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        } else if (dir->type != FS_INODE_DIR) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
        } else {
            const SnapshotState* snap = snapshot_by_id(handle->snapshot_id);
            const InodeState* table = snap ? snap->inodes : g_inodes;
            mx_u32_t ordinal = 0;
            for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
                if (!table[i].used || table[i].parent_inode != dir->inode_number) continue;
                if (ordinal == req.cookie) {
                    reply.flags = FS_READDIR_HAS_ENTRY;
                    reply.next_cookie = req.cookie + 1;
                    reply.inode_number = table[i].inode_number;
                    reply.type = table[i].type;
                    const mx_u32_t n = str_len(table[i].name, sizeof(table[i].name));
                    reply.name_len = static_cast<mx_u8_t>(n > sizeof(reply.name) ? sizeof(reply.name) : n);
                    if (n > sizeof(reply.name)) reply.flags |= FS_READDIR_TRUNCATED;
                    for (mx_u32_t j = 0; j < reply.name_len; ++j) reply.name[j] = table[i].name[j];
                }
                ++ordinal;
            }
            reply.entry_count = ordinal;
            if ((reply.flags & FS_READDIR_HAS_ENTRY) && reply.next_cookie >= ordinal) {
                reply.flags |= FS_READDIR_EOF;
            }
        }
    } else {
        reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    }

    (void)fs.send_reply(slot, FS_READDIR_REPLY, reply);
}

static void handle_mkdir(const mochi::fs::Server& fs, const RingSlot& slot) {
    mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (slot.length >= sizeof(FsMkdirRequest)) {
        const auto& req = *reinterpret_cast<const FsMkdirRequest*>(slot.payload);
        if (!req.name_len || req.name_len > sizeof(req.name)) {
            send_status_only(fs, slot, FS_MKDIR_REPLY, MX_ERR_INVALID);
            return;
        }
        mx_u32_t parent_inode = kRootInode;
        mx_u64_t snapshot_id = 0;
        bool read_only_parent = false;
        status = parent_from_token(req.parent_inode, &parent_inode, &snapshot_id, &read_only_parent);
        if (status == MX_OK && (snapshot_id != 0 || read_only_parent)) {
            status = static_cast<mx_i32_t>(MX_ERR_PERM);
        } else if (status == MX_OK && find_live_child(parent_inode, req.name, req.name_len)) {
            status = static_cast<mx_i32_t>(MX_ERR_BUSY);
        } else if (status == MX_OK) {
            InodeState* dir = alloc_inode(FS_INODE_DIR, FS_CLASS_USER_DATA,
                                          parent_inode, req.name, req.name_len,
                                          req.mode ? req.mode : 0755);
            status = dir ? MX_OK : static_cast<mx_i32_t>(MX_ERR_NOMEM);
        }
    }
    send_status_only(fs, slot, FS_MKDIR_REPLY, status);
}

static void handle_unlink(const mochi::fs::Server& fs, const RingSlot& slot) {
    mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (slot.length >= sizeof(FsUnlinkRequest)) {
        const auto& req = *reinterpret_cast<const FsUnlinkRequest*>(slot.payload);
        if (!req.name_len || req.name_len > sizeof(req.name)) {
            send_status_only(fs, slot, FS_UNLINK_REPLY, MX_ERR_INVALID);
            return;
        }
        mx_u32_t parent_inode = kRootInode;
        mx_u64_t snapshot_id = 0;
        bool read_only_parent = false;
        status = parent_from_token(req.parent_inode, &parent_inode, &snapshot_id, &read_only_parent);
        if (status == MX_OK && (snapshot_id != 0 || read_only_parent)) {
            status = static_cast<mx_i32_t>(MX_ERR_PERM);
        } else if (status == MX_OK) {
            InodeState* victim = find_live_child(parent_inode, req.name, req.name_len);
            if (!victim) {
                status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
            } else if (victim->type == FS_INODE_DIR) {
                bool non_empty = false;
                for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
                    if (g_inodes[i].used && g_inodes[i].parent_inode == victim->inode_number) {
                        non_empty = true;
                    }
                }
                status = non_empty ? static_cast<mx_i32_t>(MX_ERR_BUSY) : MX_OK;
            }
            if (status == MX_OK && victim) {
                release_inode_extents(*victim);
                zero_bytes(victim, sizeof(*victim));
                note_metadata_delta(FsMutation::DirDelete, parent_inode);
            }
        }
    }
    send_status_only(fs, slot, FS_UNLINK_REPLY, status);
}

static void handle_rename(const mochi::fs::Server& fs, const RingSlot& slot) {
    mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (slot.length >= sizeof(FsRenameRequest)) {
        const auto& req = *reinterpret_cast<const FsRenameRequest*>(slot.payload);
        InodeState* victim = find_live_child(req.old_parent, req.old_name, req.old_name_len);
        if (!victim) {
            status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        } else if (find_live_child(req.new_parent, req.new_name, req.new_name_len)) {
            status = static_cast<mx_i32_t>(MX_ERR_BUSY);
        } else {
            victim->parent_inode = req.new_parent;
            copy_name(victim->name, req.new_name, req.new_name_len);
            note_metadata_delta(FsMutation::DirInsert, req.new_parent);
            status = MX_OK;
        }
    }
    send_status_only(fs, slot, FS_RENAME_REPLY, status);
}

static void handle_promote(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsPromoteReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);

    if (slot.length >= sizeof(FsPromoteRequest)) {
        const auto& req = *reinterpret_cast<const FsPromoteRequest*>(slot.payload);
        const OpenHandle* handle = handle_state(req.handle);
        if (handle && handle->disk_backed) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_PERM);
            (void)fs.send_reply(slot, FS_PROMOTE_REPLY, reply);
            return;
        }
        const InodeState* src = handle ? any_inode(handle->snapshot_id, handle->inode_number) : nullptr;
        if (!src) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        } else if (src->type != FS_INODE_FILE) {
            reply.status = static_cast<mx_i32_t>(MX_ERR_BAD_TYPE);
        } else {
            char new_name[48]{};
            const char suffix[] = ".cow";
            mx_u32_t n = str_len(src->name, kMaxName);
            if (n > 43) n = 43;
            for (mx_u32_t i = 0; i < n; ++i) new_name[i] = src->name[i];
            for (mx_u32_t i = 0; i < 4; ++i) new_name[n + i] = suffix[i];
            InodeState* dst = alloc_inode(FS_INODE_FILE, FS_CLASS_USER_DATA,
                                          src->parent_inode, new_name,
                                          static_cast<mx_u8_t>(n + 4), 0644);
            if (!dst) {
                reply.status = static_cast<mx_i32_t>(MX_ERR_NOMEM);
            } else {
                dst->size = src->size;
                for (mx_u32_t i = 0; i < kMaxExtentsPerInode; ++i) {
                    dst->extents[i] = src->extents[i];
                    if (dst->extents[i].used) {
                        (void)inc_phys_ref(dst->extents[i].phys_block);
                    }
                }
                reply.new_handle = alloc_handle(dst->inode_number, 0, false);
                reply.status = reply.new_handle ? MX_OK : static_cast<mx_i32_t>(MX_ERR_NOMEM);
            }
        }
    }

    (void)fs.send_reply(slot, FS_PROMOTE_REPLY, reply);
}

static void pin_snapshot_extents(const SnapshotState& snap) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const InodeState& inode = snap.inodes[i];
        if (!inode.used) continue;
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            if (inode.extents[e].used) (void)inc_phys_ref(inode.extents[e].phys_block);
        }
    }
}

static void unpin_snapshot_extents(const SnapshotState& snap) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const InodeState& inode = snap.inodes[i];
        if (!inode.used) continue;
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            if (inode.extents[e].used) dec_phys_ref(inode.extents[e].phys_block);
        }
    }
}

static void handle_snapshot(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsSnapshotReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_NOMEM);

    for (mx_u32_t i = 0; i < kMaxSnapshots; ++i) {
        if (g_snapshots[i].used) continue;
        SnapshotState& snap = g_snapshots[i];
        zero_bytes(&snap, sizeof(snap));
        snap.used = true;
        snap.snapshot_id = g_generation++;
        snap.parent_snapshot_id = g_last_snapshot_id;
        fs_copy_bytes(snap.inodes, g_inodes, sizeof(g_inodes));
        pin_snapshot_extents(snap);
        g_last_snapshot_id = snap.snapshot_id;
        reply.status = MX_OK;
        reply.snapshot_id = snap.snapshot_id;
        note_metadata_delta(FsMutation::InodeWrite, 0);
        mochi::log_raw_marker("FS snapshot flush OK\n");
        break;
    }

    (void)fs.send_reply(slot, FS_SNAPSHOT_REPLY, reply);
}

static void handle_snapshot_mount(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsSnapshotMntReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (slot.length >= sizeof(FsSnapshotMntRequest)) {
        const auto& req = *reinterpret_cast<const FsSnapshotMntRequest*>(slot.payload);
        if (snapshot_by_id(req.snapshot_id)) {
            reply.dir_handle = alloc_handle(kRootInode, req.snapshot_id, true);
            reply.status = reply.dir_handle ? MX_OK : static_cast<mx_i32_t>(MX_ERR_NOMEM);
        } else {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        }
    }
    (void)fs.send_reply(slot, FS_SNAPSHOT_MNT_REPLY, reply);
}

static void handle_snapshot_drop(const mochi::fs::Server& fs, const RingSlot& slot) {
    mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    if (slot.length >= sizeof(mx_u64_t)) {
        mx_u64_t snapshot_id = 0;
        fs_copy_bytes(&snapshot_id, slot.payload, sizeof(snapshot_id));
        SnapshotState* snap = snapshot_by_id(snapshot_id);
        if (snap) {
            unpin_snapshot_extents(*snap);
            zero_bytes(snap, sizeof(*snap));
            status = MX_OK;
        } else {
            status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        }
    }
    send_status_only(fs, slot, FS_SYNC_REPLY, status);
}

static void handle_slot(const mochi::fs::Server& fs, const RingSlot& slot) {
    switch (slot.type) {
    case FS_OPEN:
        handle_open(fs, slot);
        break;
    case FS_CLOSE: {
        mx_i32_t status = MX_ERR_INVALID;
        if (slot.length == sizeof(fs_handle_t)) {
            fs_handle_t token = 0;
            fs_copy_bytes(&token, slot.payload, sizeof(token));
            auto* h = handle_state(token);
            if (h && token != kRootHandle) { *h = {}; status = MX_OK; }
        }
        send_status_only(fs, slot, FS_SYNC_REPLY, status);
        break;
    }
    case FS_READ:
        handle_read(fs, slot);
        break;
    case FS_WRITE:
        handle_write(fs, slot);
        break;
    case FS_READDIR:
        handle_readdir(fs, slot);
        break;
    case FS_MKDIR:
        handle_mkdir(fs, slot);
        break;
    case FS_UNLINK:
        handle_unlink(fs, slot);
        break;
    case FS_RENAME:
        handle_rename(fs, slot);
        break;
    case FS_PROMOTE:
        handle_promote(fs, slot);
        break;
    case FS_SNAPSHOT:
        handle_snapshot(fs, slot);
        break;
    case FS_SNAPSHOT_MNT:
        handle_snapshot_mount(fs, slot);
        break;
    case FS_SNAPSHOT_DROP:
        handle_snapshot_drop(fs, slot);
        break;
    case FS_SYNC:
        send_status_only(fs, slot, FS_SYNC_REPLY, MX_ERR_NOT_IMPLEMENTED);
        break;
    case FS_PREFETCH:
        break;
    default:
        (void)fs.send_error(slot, static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL));
        break;
    }
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "io.fs");

    register_service_arenas();
    format_memory_fs();

    if (!g_sessions.map(boot)) mochi::boot::invalid_boot();
    mx_u32_t blk_req_order = kRingOrder;
    mx_u32_t blk_reply_order = kRingOrder;
    RingPage* blk_req = map_ring_from_boot(boot, "blk.req.ring", kVaBlkReqRing, &blk_req_order);
    RingPage* blk_reply = map_ring_from_boot(boot, "blk.reply.ring", kVaBlkReplyRing, &blk_reply_order);
    const CapHandle blk_req_db = mochi::boot::handle(boot, "blk.req.doorbell");
    const CapHandle blk_reply_db = mochi::boot::handle(boot, "blk.reply.doorbell");
    if (blk_req && blk_reply && blk_req_db != MX_NULL_HANDLE && blk_reply_db != MX_NULL_HANDLE) {
        g_blk_client = {{blk_req, blk_reply, blk_req_order, blk_req_db, blk_reply_db}};
        g_blk_available = g_blk_client.valid();
        mochi::log_raw_marker("io.fs: block endpoint ready\n");
    } else {
        g_blk_client = {};
        g_blk_available = false;
        mochi::log_raw_marker("io.fs: no block endpoint; running memory backend\n");
    }

    const mx_handle_t blk_dma = mochi::boot::handle(boot, "virtio.blk.dma");
    MxCapInfo dma_info{};
    if (blk_dma != MX_NULL_HANDLE &&
        mx_cap_info(blk_dma, &dma_info) == MX_OK &&
        dma_info.size >= BLK_DMA_CLIENT_OFFSET + kBlockSize &&
        mx_map_page(blk_dma, kVaBlkDma, MX_RIGHT_READ | MX_RIGHT_WRITE) == MX_OK) {
        g_blk_dma = reinterpret_cast<mx_u8_t*>(kVaBlkDma);
        g_blk_dma_size = dma_info.size;
        (void)g_blk_dma_size;
        if (!mount_disk_fs()) {
            mochi::log_raw_marker("io.fs: no valid midix-fs disk; memory backend only\n");
        }
    } else if (g_blk_available) {
        mochi::log_raw_marker("io.fs: no block dma arena; disk mount skipped\n");
    }

    for (mx_u32_t i = 0; i < g_sessions.directory->count; ++i) g_sessions.refresh(i, close_session);
    mochi::log_raw_marker("io.fs: read-only root and volatile /tmp ready\n");
    mochi::boot::ready(boot);
    while (true) {
        bool progress = false;
        for (mx_u32_t i = 0; i < g_sessions.directory->count; ++i) {
            if (!g_sessions.refresh(i, close_session)) continue;
            g_session = i;
            const mochi::fs::Server fs{g_sessions.endpoints[i]};
            RingSlot slot{};
            // Bound each client's work so a full ring cannot starve its peers.
            for (mx_u32_t n = 0; n < 8 && fs.try_recv(&slot); ++n) {
                handle_slot(fs, slot);
                progress = true;
            }
        }
        if (!progress) mx_yield();
    }
}

#pragma once
// Midix filesystem and block-driver protocol ABI.
//
// All request/reply payload structs below are sized for MxRingSlot::payload,
// which is 48 bytes on the V0 64-byte control-ring slot.

#include "midix/abi/types.h"
#include "midix/abi/status.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_FS  = 0x46530000u; // 'FS\0\0'
constexpr mx_u32_t MX_PROTOCOL_BLK = 0x424C4B00u; // 'BLK\0'
constexpr mx_u16_t MX_FS_VERSION = 1;

using fs_handle_t = mx_u32_t;
using mx_cap_handle_t = mx_handle_t;

constexpr mx_u32_t FS_BLOCK_SIZE = 4096;
constexpr mx_u32_t FS_INLINE_READ_MAX = 32;
constexpr mx_u32_t FS_INLINE_WRITE_MAX = 24;

constexpr mx_u32_t FS_MAGIC = 0xFEED'FACEu;
constexpr mx_u32_t FS_SUPERBLOCK_VERSION = 1;

enum FsInodeType : mx_u16_t {
    FS_INODE_FILE    = 1,
    FS_INODE_DIR     = 2,
    FS_INODE_SYMLINK = 3,
};

enum FsInodeClass : mx_u16_t {
    FS_CLASS_CONTENT_STORE = 1,
    FS_CLASS_USER_DATA     = 2,
};

enum FsExtentFlags : mx_u16_t {
    FS_EXTENT_COLD   = 1u << 0,
    FS_EXTENT_PINNED = 1u << 1,
    FS_EXTENT_COW    = 1u << 2,
};

enum FsPhysExtentFlags : mx_u32_t {
    FS_PHYS_COW_PROTECTED = 1u << 0,
};

enum FsSuperblockFlags : mx_u32_t {
    FS_FLAG_DIRTY = 1u << 0,
};

enum FsOpenFlags : mx_u32_t {
    FS_OPEN_READ  = 1u << 0,
    FS_OPEN_WRITE = 1u << 1,
    FS_OPEN_CREATE = 1u << 2,
    FS_OPEN_TRUNC = 1u << 3,
    FS_OPEN_LONG_NAME = 1u << 4,
};

enum FsReadFlags : mx_u32_t {
    FS_READ_ZERO_COPY = 1u << 0,
    FS_READ_DIRECT    = 1u << 1,
};

enum FsWriteFlags : mx_u32_t {
    FS_WRITE_ZERO_COPY = 1u << 0,
};

enum FsSnapshotFlags : mx_u32_t {
    FS_SNAPSHOT_RO = 1u << 0,
};

enum FsSnapshotLeafFlags : mx_u32_t {
    FS_SNAPSHOT_RO_MOUNTABLE = 1u << 0,
};

enum FsPrefetchType : mx_u32_t {
    FS_PREFETCH_SEQ    = 1,
    FS_PREFETCH_RANDOM = 2,
    FS_PREFETCH_WARMUP = 3,
};

enum FsMsgType : mx_u32_t {
    FS_OPEN               = 1,
    FS_OPEN_REPLY         = 2,
    FS_CLOSE              = 3,
    FS_READ               = 4,
    FS_READ_REPLY         = 5,
    FS_WRITE              = 6,
    FS_WRITE_REPLY        = 7,
    FS_READDIR            = 8,
    FS_READDIR_REPLY      = 9,
    FS_MKDIR              = 10,
    FS_MKDIR_REPLY        = 11,
    FS_UNLINK             = 12,
    FS_UNLINK_REPLY       = 13,
    FS_RENAME             = 14,
    FS_RENAME_REPLY       = 15,
    FS_PROMOTE            = 16,
    FS_PROMOTE_REPLY      = 17,
    FS_SNAPSHOT           = 18,
    FS_SNAPSHOT_REPLY     = 19,
    FS_SNAPSHOT_MNT       = 20,
    FS_SNAPSHOT_MNT_REPLY = 21,
    FS_SNAPSHOT_DROP      = 22,
    FS_SYNC               = 23,
    FS_SYNC_REPLY         = 24,
    FS_PREFETCH           = 25,
    FS_ERROR              = 26,
    // Status-only replies that used to reuse FS_SYNC_REPLY. Appended so a
    // client waiting for an FS_SYNC_REPLY can never mistake a close/drop
    // acknowledgement for a sync result.
    FS_CLOSE_REPLY        = 27,
    FS_SNAPSHOT_DROP_REPLY = 28,
};

using FsFrame = MxRingSlot;
static_assert(sizeof(FsFrame) == 64, "FS frame must be one V0 control-ring slot");
static_assert(MX_RING_INLINE_PAYLOAD == 48, "FS payload budget changed");

struct FsSuperblock {
    mx_u32_t magic;
    mx_u32_t version;
    mx_u64_t total_blocks;
    mx_u64_t block_size;
    mx_u64_t inode_table_root;
    mx_u64_t extent_bitmap_root;
    mx_u64_t phys_refcount_root;
    mx_u64_t free_extent_count;
    mx_u64_t snapshot_tree_root;
    mx_u64_t journal_start_block;
    mx_u64_t journal_block_count;
    mx_u64_t first_data_block;
    mx_u64_t current_generation;
    mx_u32_t flags;
    mx_u32_t reserved0;
    mx_u8_t uuid[16];
    mx_u8_t padding[3976];
};
static_assert(sizeof(FsSuperblock) == 4096, "FS superblock must be exactly one block");

struct FsCheckpoint {
    mx_u64_t lsn;
    mx_u64_t inode_table_root;
    mx_u64_t extent_bitmap_root;
    mx_u64_t phys_refcount_root;
    mx_u64_t snapshot_tree_root;
    mx_u64_t current_generation;
    mx_u64_t timestamp_ns;
    mx_u32_t crc32;
    mx_u32_t reserved;
    mx_u8_t padding[4032];
};
static_assert(sizeof(FsCheckpoint) == 4096, "FS checkpoint must be exactly one block");

struct BTreeNodeHeader {
    mx_u16_t node_type;
    mx_u16_t entry_count;
    mx_u32_t flags;
    mx_u64_t generation;
    mx_u64_t parent_block;
    mx_u64_t next_sibling;
    mx_u8_t reserved[32];
};
static_assert(sizeof(BTreeNodeHeader) == 64, "B+tree node header must be 64 bytes");

struct BTreeInternalEntry {
    mx_u64_t max_key;
    mx_u64_t child_block;
    mx_u8_t reserved[48];
};
static_assert(sizeof(BTreeInternalEntry) == 64, "B+tree internal entry must be 64 bytes");

struct InodeLeafEntry {
    mx_u32_t inode_number;
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u64_t size;
    mx_u64_t extent_tree_root;
    mx_u32_t nlink;
    mx_u32_t mode;
    mx_u64_t ctime_ns;
    mx_u64_t mtime_ns;
    mx_u8_t reserved[16];
};
static_assert(sizeof(InodeLeafEntry) == 64, "inode leaf entry must be 64 bytes");

struct ExtentLeafEntry {
    mx_u64_t logical_block;
    mx_u64_t phys_block;
    mx_u64_t length_blocks;
    mx_u64_t generation;
    mx_u16_t flags;
    mx_u16_t reserved;
    mx_u32_t data_crc32;
    mx_u32_t reserved0;
    mx_u8_t pad[20];
};
static_assert(sizeof(ExtentLeafEntry) == 64, "extent leaf entry must be 64 bytes");

struct PhysExtentLeafEntry {
    mx_u64_t phys_block;
    mx_u64_t length_blocks;
    mx_u32_t refcount;
    mx_u32_t flags;
    mx_u8_t reserved[40];
};
static_assert(sizeof(PhysExtentLeafEntry) == 64, "phys extent leaf entry must be 64 bytes");

struct DirLeafEntry {
    mx_u64_t compound_key;
    mx_u32_t child_inode;
    mx_u8_t name_len;
    mx_u8_t file_type;
    mx_u16_t reserved;
    char name[48];
};
static_assert(sizeof(DirLeafEntry) == 64, "directory leaf entry must be 64 bytes");

struct FreeSpaceLeafEntry {
    mx_u64_t range_start;
    mx_u64_t range_end;
    mx_u64_t bitmap_block;
    mx_u8_t reserved[40];
};
static_assert(sizeof(FreeSpaceLeafEntry) == 64, "free-space leaf entry must be 64 bytes");

struct SnapshotLeafEntry {
    mx_u64_t snapshot_id;
    mx_u64_t timestamp_ns;
    mx_u64_t inode_table_root;
    mx_u64_t extent_bitmap_root;
    mx_u64_t phys_refcount_root;
    mx_u64_t parent_snapshot_id;
    mx_u32_t flags;
    mx_u32_t reserved;
    mx_u8_t pad[8];
};
static_assert(sizeof(SnapshotLeafEntry) == 64, "snapshot leaf entry must be 64 bytes");

struct FsOpenRequest {
    mx_u32_t parent_inode;
    mx_u32_t flags;
    mx_u8_t name_len;
    mx_u8_t pad[3];
    char name[36];
};
static_assert(sizeof(FsOpenRequest) == 48, "FS open request must fit one payload");

struct FsOpenReply {
    mx_i32_t status;
    fs_handle_t handle;
    mx_u64_t inode_number;
    mx_u64_t size;
    mx_u16_t type;
    mx_u16_t inode_class;
    mx_u32_t mode;
    mx_u8_t reserved[16];
};
static_assert(sizeof(FsOpenReply) == 48, "FS open reply must fit one payload");

struct FsReadRequest {
    fs_handle_t handle;
    mx_u32_t flags;
    mx_u64_t offset;
    mx_u32_t length;
    mx_u32_t reserved;
};
static_assert(sizeof(FsReadRequest) == 24, "FS read request size drift");

struct FsReadReply {
    mx_i32_t status;
    mx_u32_t length_read;
    mx_cap_handle_t arena_cap;
    mx_u8_t inline_data[32];
};
static_assert(sizeof(FsReadReply) == 48, "FS read reply must fit one payload");

struct FsWriteRequest {
    fs_handle_t handle;
    mx_u32_t flags;
    mx_u64_t offset;
    mx_u32_t length;
    mx_u32_t reserved;
    mx_u8_t payload[24];
};
static_assert(sizeof(FsWriteRequest) == 48, "FS write request must fit one payload");

struct FsWriteReply {
    mx_i32_t status;
    mx_u32_t bytes_written;
};
static_assert(sizeof(FsWriteReply) == 8, "FS write reply size drift");

struct FsReaddirRequest {
    fs_handle_t handle;
    mx_u32_t cookie;
    mx_u32_t flags;
    mx_u32_t reserved;
};
static_assert(sizeof(FsReaddirRequest) == 16, "FS readdir request size drift");

enum FsReaddirFlags : mx_u8_t {
    FS_READDIR_HAS_ENTRY = 1u << 0,
    FS_READDIR_EOF       = 1u << 1,
    FS_READDIR_TRUNCATED = 1u << 2,
};

struct FsReaddirReply {
    mx_i32_t status;
    mx_u32_t entry_count;
    mx_u32_t next_cookie;
    mx_u32_t inode_number;
    mx_u16_t type;
    mx_u8_t name_len;
    mx_u8_t flags;
    char name[28];
};
static_assert(sizeof(FsReaddirReply) == 48, "FS readdir reply must fit one payload");

struct FsMkdirRequest {
    mx_u32_t parent_inode;
    mx_u32_t flags;
    mx_u32_t mode;
    mx_u8_t name_len;
    mx_u8_t pad[3];
    char name[32];
};
static_assert(sizeof(FsMkdirRequest) == 48, "FS mkdir request must fit one payload");

struct FsUnlinkRequest {
    mx_u32_t parent_inode;
    mx_u32_t flags;
    mx_u8_t name_len;
    mx_u8_t pad[3];
    char name[36];
};
static_assert(sizeof(FsUnlinkRequest) == 48, "FS unlink request must fit one payload");

struct FsRenameRequest {
    mx_u32_t old_parent;
    mx_u32_t new_parent;
    mx_u8_t old_name_len;
    mx_u8_t new_name_len;
    mx_u16_t flags;
    char old_name[20];
    char new_name[16];
};
static_assert(sizeof(FsRenameRequest) == 48, "FS rename request must fit one payload");

struct FsPromoteRequest {
    fs_handle_t handle;
    mx_u32_t flags;
};
static_assert(sizeof(FsPromoteRequest) == 8, "FS promote request size drift");

struct FsPromoteReply {
    mx_i32_t status;
    fs_handle_t new_handle;
};
static_assert(sizeof(FsPromoteReply) == 8, "FS promote reply size drift");

struct FsSnapshotRequest {
    mx_u32_t flags;
    mx_u8_t label[12];
};
static_assert(sizeof(FsSnapshotRequest) == 16, "FS snapshot request size drift");

struct FsSnapshotReply {
    mx_i32_t status;
    mx_u32_t reserved;
    mx_u64_t snapshot_id;
};
static_assert(sizeof(FsSnapshotReply) == 16, "FS snapshot reply size drift");

struct FsSnapshotMntRequest {
    mx_u64_t snapshot_id;
    mx_u32_t flags;
    mx_u32_t reserved;
};
static_assert(sizeof(FsSnapshotMntRequest) == 16, "FS snapshot mount request size drift");

struct FsSnapshotMntReply {
    mx_i32_t status;
    fs_handle_t dir_handle;
};
static_assert(sizeof(FsSnapshotMntReply) == 8, "FS snapshot mount reply size drift");

struct FsPrefetchHint {
    fs_handle_t handle;
    mx_u32_t type;
    mx_u64_t offset;
    mx_u32_t length;
    mx_u32_t reserved;
};
static_assert(sizeof(FsPrefetchHint) == 24, "FS prefetch hint size drift");

enum JournalRecordType : mx_u32_t {
    JRN_EXTENT_ALLOC = 1,
    JRN_EXTENT_FREE  = 2,
    JRN_INODE_WRITE  = 3,
    JRN_DIR_INSERT   = 4,
    JRN_DIR_DELETE   = 5,
    JRN_PHYS_REF_INC = 6,
    JRN_PHYS_REF_DEC = 7,
};

struct JournalRecord {
    mx_u64_t lsn;
    mx_u32_t type;
    mx_u32_t target_block;
    mx_u64_t timestamp_ns;
    mx_u32_t record_crc32;
    mx_u32_t reserved;
    mx_u8_t data[4064];
};
static_assert(sizeof(JournalRecord) == 4096, "journal record must be exactly one block");

struct JournalCommit {
    mx_u64_t lsn;
    mx_u64_t first_record_lsn;
    mx_u32_t record_count;
    mx_u32_t batch_crc32;
    mx_u64_t timestamp_ns;
    mx_u8_t pad[4064];
};
static_assert(sizeof(JournalCommit) == 4096, "journal commit must be exactly one block");

enum BlkMsgType : mx_u32_t {
    BLK_READ  = 1,
    BLK_WRITE = 2,
    BLK_FLUSH = 3,
    BLK_REPLY = 4,
};

constexpr mx_u32_t BLK_DMA_CLIENT_OFFSET = 0x0001'0000u;

struct BlkRequest {
    mx_u32_t type;
    mx_u32_t flags;
    mx_u64_t sector;
    mx_u32_t sector_count;
    mx_u32_t reserved;
    mx_cap_handle_t arena_cap;
    mx_u32_t arena_offset;
    mx_u32_t reserved2;
};
static_assert(sizeof(BlkRequest) == 40, "block request must fit one payload");

struct BlkReply {
    mx_i32_t status;
    mx_u32_t sectors_transferred;
};
static_assert(sizeof(BlkReply) == 8, "block reply size drift");

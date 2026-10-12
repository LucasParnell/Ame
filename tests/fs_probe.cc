// Production filesystem state and handlers with an in-memory block-device seam.
#include <cassert>
#include "../userland/fs/fs_core.cc"
#include "../userland/fs/fs_handles.cc"
#include "../userland/fs/fs_snapshot.cc"
#include "../userland/fs/fs_durable.cc"

static mx_u8_t disk[128][kBlockSize];
static unsigned reads[128];
static unsigned fail_block = 128;
static unsigned flushes;
static mx_u32_t flushed_states[3];
static bool mounted = true;
bool disk_mounted() { return mounted; }
bool disk_read_block(mx_u64_t block, mx_u8_t* out) {
    if (block >= 128 || (++reads[block] == 3 && block == fail_block)) return false;
    return libk::copy_exact(libk::as_bytes(out, kBlockSize), libk::as_bytes(disk[block]));
}
bool disk_write_block(mx_u64_t block, const mx_u8_t* data) {
    return block < 128 && libk::copy_exact(libk::as_bytes(disk[block]), libk::as_bytes(data, kBlockSize));
}
mx_i32_t disk_flush() {
    assert(flushes < 3);
    flushed_states[flushes++] = reinterpret_cast<const DurableHeader*>(disk[0])->state;
    return MX_OK;
}
bool disk_durable_region(mx_u64_t* start, mx_u64_t* count) { *start = 0; *count = 128; return true; }
bool disk_inode(mx_u32_t, InodeLeafEntry*) { return false; }
bool disk_dir_lookup(const InodeLeafEntry&, mx_u32_t, const char*, mx_u8_t, InodeLeafEntry*) { return false; }
fs_handle_t alloc_disk_handle(const InodeLeafEntry&) { return 0; }
mx_i32_t disk_read_file(const OpenHandle&, mx_u64_t, mx_u32_t, FsReadReply*) { return MX_ERR_NOT_FOUND; }
mx_i32_t disk_readdir(const OpenHandle&, const FsReaddirRequest&, FsReaddirReply*) { return MX_ERR_NOT_FOUND; }

static InodeState* file(const char* name) {
    auto* inode = alloc_inode(FS_INODE_FILE, FS_CLASS_USER_DATA, g_data_inode, name,
                             libk::str::length(name));
    assert(inode);
    return inode;
}
static void sync() {
    flushes = 0;
    assert(durable_sync() == MX_OK && flushes == 3);
    assert(flushed_states[0] == kDurableStateDirty && flushed_states[1] == kDurableStateDirty);
    assert(flushed_states[2] == kDurableStateCommitted);
    libk::zero(reads);
}
static void rename_guards() {
    struct Storage { RingPage page; RingSlot slots[2]; } storage{};
    mochi::fs::Server server{{&storage.page, &storage.page, 1, 1, 1}, 100};
    auto* inode = file("old");
    const auto parent = alloc_handle(g_data_inode, 0, false);
    FsRenameRequest request{};
    request.old_parent = parent;
    request.new_parent = parent;
    request.old_name_len = 3;
    request.new_name_len = 3;
    libk::copy_n(libk::as_bytes(request.old_name), libk::as_bytes("old", 3), 3);
    libk::copy_n(libk::as_bytes(request.new_name), libk::as_bytes("new", 3), 3);
    RingSlot slot{}, reply{};
    auto call = [&] {
        mochi::message_put(&slot, FS_RENAME, request);
        handle_rename(server, slot);
        assert(ring_pop(&storage.page, 1, &reply));
        mx_i32_t status = MX_OK;
        assert(mochi::message_get(reply, &status, FS_RENAME_REPLY));
        return status;
    };
    ++g_session;
    assert(call() != MX_OK && libk::str::equal(inode->name, "old"));
    --g_session;
    request.new_name_len = sizeof(request.new_name) + 1;
    assert(call() == MX_ERR_INVALID);
    request.new_name_len = 3;
    assert(call() == MX_OK && libk::str::equal(inode->name, "new"));
}
static void access_guards() {
    format_memory_fs();
    mounted = false;
    struct Storage { RingPage page; RingSlot slots[2]; } storage{};
    mochi::fs::Server server{{&storage.page, &storage.page, 1, 1, 1}, 100};
    auto open = [&](fs_handle_t parent, const char* name, mx_u32_t flags) {
        FsOpenRequest request{};
        request.parent_inode = parent;
        request.flags = flags;
        request.name_len = mochi::fs::copy_inline_name(request.name, name);
        RingSlot slot{}, response{};
        mochi::message_put(&slot, FS_OPEN, request);
        handle_open(server, slot);
        assert(ring_pop(&storage.page, 1, &response));
        FsOpenReply reply{};
        assert(mochi::message_get(response, &reply, FS_OPEN_REPLY));
        return reply;
    };
    const auto data = open(kRootHandle, "data", FS_OPEN_READ);
    assert(data.status == MX_OK && data.access == (FS_ACCESS_ENUMERATE | FS_ACCESS_MODIFY));
    auto* child = alloc_inode(FS_INODE_DIR, FS_CLASS_USER_DATA, g_data_inode, "child", 5);
    assert(child);
    const auto read_only = alloc_handle(g_data_inode, 0, true);
    const auto restricted_child = open(read_only, "child", FS_OPEN_READ);
    assert(restricted_child.status == MX_OK && restricted_child.access == FS_ACCESS_ENUMERATE);
    assert(open(restricted_child.handle, "forbidden", FS_OPEN_CREATE | FS_OPEN_WRITE).status != MX_OK);
    const auto writable = open(data.handle, "file", FS_OPEN_CREATE | FS_OPEN_WRITE);
    assert(writable.status == MX_OK && writable.access == (FS_ACCESS_READ | FS_ACCESS_WRITE));
    const auto read = open(data.handle, "file", FS_OPEN_READ);
    assert(read.status == MX_OK && read.access == FS_ACCESS_READ);
    assert(open(kRootHandle, kHelloName, FS_OPEN_WRITE).status == MX_ERR_PERM);

    // A snapshot root's /data is the captured directory, never the live mount.
    FsSnapshotRequest request{};
    RingSlot slot{}, response{};
    mochi::message_put(&slot, FS_SNAPSHOT, request);
    handle_snapshot(server, slot);
    assert(ring_pop(&storage.page, 1, &response));
    FsSnapshotReply snapshot{};
    assert(mochi::message_get(response, &snapshot, FS_SNAPSHOT_REPLY));
    const auto snapshot_root = alloc_handle(kRootInode, snapshot.snapshot_id, true);
    const auto snapshot_data = open(snapshot_root, "data", FS_OPEN_READ);
    assert(snapshot_data.status == MX_OK && snapshot_data.access == FS_ACCESS_ENUMERATE);
    assert(handle_state(snapshot_data.handle)->snapshot_id == snapshot.snapshot_id);
    assert(open(snapshot_data.handle, "forbidden", FS_OPEN_CREATE | FS_OPEN_WRITE).status != MX_OK);
    mounted = true;
}

int main() {
    format_memory_fs();
    auto* first = file("first");
    const mx_u8_t data[] = {1, 2, 3};
    assert(write_inline(*first, ~mx_u64_t{0}, data, sizeof(data)) == MX_ERR_OVERFLOW);
    assert(first->size == 0 && !find_extent_const(*first, 0));
    assert(write_inline(*first, 0, nullptr, 1) == MX_ERR_INVALID);
    assert(write_inline(*first, 0, data, sizeof(data)) == MX_OK);
    auto* second = file("second");
    assert(write_inline(*second, 0, data, sizeof(data)) == MX_OK);
    sync();
    // Historical mode bits have no authority and must not prevent replay.
    auto& legacy = *reinterpret_cast<DurableRecord*>(disk[kDurableFirstRecordBlock]);
    legacy.legacy_mode = 0644;
    legacy.record_crc32 = 0;
    legacy.record_crc32 = libk::crc32_ieee(libk::as_bytes(&legacy, sizeof(legacy)));
    format_memory_fs();
    fail_block = 4;
    assert(!durable_load());
    assert(!find_live_child(g_data_inode, "first", 5));
    assert(!find_live_child(g_data_inode, "second", 6));
    assert(verify_refcount_index() == MX_OK);
    fail_block = 128;
    libk::zero(reads);
    assert(durable_load());
    assert(find_live_child(g_data_inode, "second", 6)->size == sizeof(data));

    format_memory_fs();
    auto* sparse = file("sparse");
    assert(write_inline(*sparse, 20 * kBlockSize + 1, data, sizeof(data)) == MX_OK);
    sparse->inode_class = FS_CLASS_CONTENT_STORE;
    sync();
    format_memory_fs();
    assert(durable_load());
    sparse = find_live_child(g_data_inode, "sparse", 6);
    assert(sparse && sparse->size == 20 * kBlockSize + 4);
    assert(sparse->inode_class == FS_CLASS_CONTENT_STORE && verify_refcount_index() == MX_OK);

    format_memory_fs();
    sync(); // an empty /data checkpoint is valid
    format_memory_fs();
    assert(durable_load());
    rename_guards();
    access_guards();
}

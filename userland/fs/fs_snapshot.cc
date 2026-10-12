// userland/fs/fs_snapshot.cc — copy-on-write snapshots.
//
// A snapshot copies the in-memory inode table and pins every referenced phys
// block, so later CoW writes diverge instead of mutating the snapshot's data.
// Dropping a snapshot releases exactly those pins.

#include "fs_internal.h"
#include "mochi/log.h"

SnapshotState* snapshot_by_id(mx_u64_t snapshot_id) {
    if (snapshot_id == 0) {
        return nullptr;
    }
    for (mx_u32_t i = 0; i < kMaxSnapshots; ++i) {
        if (g_snapshots[i].used && g_snapshots[i].snapshot_id == snapshot_id) {
            return &g_snapshots[i];
        }
    }
    return nullptr;
}

static const InodeState* snapshot_inode(mx_u64_t snapshot_id, mx_u32_t inode_number) {
    const SnapshotState* snap = snapshot_by_id(snapshot_id);
    if (!snap) {
        return nullptr;
    }
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        if (snap->inodes[i].used && snap->inodes[i].inode_number == inode_number) {
            return &snap->inodes[i];
        }
    }
    return nullptr;
}

const InodeState* any_inode(mx_u64_t snapshot_id, mx_u32_t inode_number) {
    if (snapshot_id) {
        return snapshot_inode(snapshot_id, inode_number);
    }
    return live_inode(inode_number);
}

static void pin_snapshot_extents(const SnapshotState& snap) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const InodeState& inode = snap.inodes[i];
        if (!inode.used) {
            continue;
        }
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            if (inode.extents[e].used) {
                (void)inc_phys_ref(inode.extents[e].phys_block);
            }
        }
    }
}

static void unpin_snapshot_extents(const SnapshotState& snap) {
    for (mx_u32_t i = 0; i < kMaxInodes; ++i) {
        const InodeState& inode = snap.inodes[i];
        if (!inode.used) {
            continue;
        }
        for (mx_u32_t e = 0; e < kMaxExtentsPerInode; ++e) {
            if (inode.extents[e].used) {
                dec_phys_ref(inode.extents[e].phys_block);
            }
        }
    }
}

void handle_snapshot(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsSnapshotReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_NOMEM);

    for (mx_u32_t i = 0; i < kMaxSnapshots; ++i) {
        if (g_snapshots[i].used) {
            continue;
        }
        SnapshotState& snap = g_snapshots[i];
        libk::zero(snap);
        snap.used = true;
        snap.snapshot_id = g_generation++;
        snap.parent_snapshot_id = g_last_snapshot_id;
        libk::copy_exact(libk::as_bytes(snap.inodes), libk::as_bytes(g_inodes));
        pin_snapshot_extents(snap);
        g_last_snapshot_id = snap.snapshot_id;
        reply.status = MX_OK;
        reply.snapshot_id = snap.snapshot_id;
        mark_metadata_dirty();
        mochi::log_raw_marker("FS snapshot flush OK\n");
        break;
    }

    (void)fs.send_reply(slot, FS_SNAPSHOT_REPLY, reply);
}

void handle_snapshot_mount(const mochi::fs::Server& fs, const RingSlot& slot) {
    FsSnapshotMntReply reply{};
    reply.status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    FsSnapshotMntRequest req{};
    if (mochi::message_get(slot, &req)) {
        if (snapshot_by_id(req.snapshot_id)) {
            reply.dir_handle = alloc_handle(kRootInode, req.snapshot_id, true);
            reply.status = reply.dir_handle ? MX_OK : static_cast<mx_i32_t>(MX_ERR_NOMEM);
        } else {
            reply.status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        }
    }
    (void)fs.send_reply(slot, FS_SNAPSHOT_MNT_REPLY, reply);
}

void handle_snapshot_drop(const mochi::fs::Server& fs, const RingSlot& slot) {
    mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_INVALID);
    mx_u64_t snapshot_id = 0;
    if (mochi::message_get(slot, &snapshot_id)) {
        SnapshotState* snap = snapshot_by_id(snapshot_id);
        if (snap) {
            unpin_snapshot_extents(*snap);
            libk::zero(*snap);
            status = MX_OK;
        } else {
            status = static_cast<mx_i32_t>(MX_ERR_NOT_FOUND);
        }
    }
    (void)fs.send_reply(slot, FS_SNAPSHOT_DROP_REPLY, status);
}

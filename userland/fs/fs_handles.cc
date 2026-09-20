// userland/fs/fs_handles.cc — open-handle table and request handlers.
//
// Handle tokens are session-owned: a token carries the handle slot and its
// generation, and the table records which client session (slot index + slot
// generation) opened it. handle_state accepts a token only when both the handle
// generation and the session generation match the client currently being served,
// so a token minted before a client restart can never be replayed.

#include "fs_internal.h"

fs_handle_t handle_token(mx_u32_t index) {
    g_handles[index].owner = g_session;
    g_handles[index].owner_generation = g_session_generation;
    return (++g_handle_generations[index] << 8) | index;
}

fs_handle_t alloc_handle(mx_u32_t inode_number, mx_u64_t snapshot_id, bool read_only) {
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

void close_session(mx_u32_t session) {
    for (mx_u32_t i = 2; i < kMaxHandles; ++i) {
        if (g_handles[i].used && g_handles[i].owner == session) g_handles[i] = {};
    }
}

static OpenHandle* handle_state(fs_handle_t handle) {
    if (handle == kRootHandle) return &g_handles[kRootHandle];
    const mx_u32_t index = handle & 255;
    if (index < 2 || index >= kMaxHandles || !g_handles[index].used ||
        (handle >> 8) != g_handle_generations[index] ||
        g_handles[index].owner != g_session ||
        g_handles[index].owner_generation != g_session_generation) {
        return nullptr;
    }
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

void send_status_only(const mochi::fs::Server& fs, const RingSlot& req,
                      mx_u32_t reply_type, mx_i32_t status) {
    (void)fs.send_payload(req, reply_type, &status, sizeof(status));
}

void handle_close(const mochi::fs::Server& fs, const RingSlot& slot) {
    mx_i32_t status = MX_ERR_INVALID;
    if (slot.length == sizeof(fs_handle_t)) {
        fs_handle_t token = 0;
        fs_copy_bytes(&token, slot.payload, sizeof(token));
        auto* h = handle_state(token);
        if (h && token != kRootHandle) { *h = {}; status = MX_OK; }
    }
    send_status_only(fs, slot, FS_SYNC_REPLY, status);
}

void handle_open(const mochi::fs::Server& fs, const RingSlot& slot) {
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
        // Disk-backed parents are read-only. /tmp is redirected away from the
        // disk entirely and served from the volatile in-memory table.
        if (disk_mounted() && !tmp_mount && parent_handle &&
            (root_parent || parent_handle->disk_backed)) {
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

void handle_read(const mochi::fs::Server& fs, const RingSlot& slot) {
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

void handle_write(const mochi::fs::Server& fs, const RingSlot& slot) {
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

void handle_readdir(const mochi::fs::Server& fs, const RingSlot& slot) {
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

void handle_mkdir(const mochi::fs::Server& fs, const RingSlot& slot) {
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

void handle_unlink(const mochi::fs::Server& fs, const RingSlot& slot) {
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

void handle_rename(const mochi::fs::Server& fs, const RingSlot& slot) {
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

void handle_promote(const mochi::fs::Server& fs, const RingSlot& slot) {
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

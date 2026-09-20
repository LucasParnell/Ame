#pragma once
// mochi/fs.h — small helpers for the io.fs and drv.blk.virtio ring protocols.

#include "mochi/endpoint.h"
#include "midix/abi/protocols/fs.h"

namespace mochi::fs {

static inline void copy_bytes(void* dst, const void* src, mx_u32_t len) {
    auto* d = static_cast<mx_u8_t*>(dst);
    const auto* s = static_cast<const mx_u8_t*>(src);
    for (mx_u32_t i = 0; i < len; ++i) d[i] = s[i];
}

static inline mx_u32_t bounded_strlen(const char* s, mx_u32_t limit) {
    mx_u32_t n = 0;
    if (!s) return 0;
    while (n < limit && s[n]) ++n;
    return n;
}

static inline void copy_inline_name(char* dst, mx_u32_t dst_len,
                                    const char* src, mx_u8_t* out_len) {
    mx_u32_t n = 0;
    if (src) {
        while (n + 1 < dst_len && src[n]) {
            dst[n] = src[n];
            ++n;
        }
    }
    if (dst_len != 0) dst[n] = '\0';
    if (out_len) *out_len = static_cast<mx_u8_t>(n);
}

template <typename T>
static inline bool payload_from_slot(const RingSlot& slot, T* out) {
    if (!out || slot.length < sizeof(T)) return false;
    copy_bytes(out, slot.payload, sizeof(T));
    return true;
}

struct Client {
    SpscEndpoint ep;

    bool valid() const {
        return ep.valid();
    }

    bool try_open(mx_u32_t parent_inode, const char* name, mx_u32_t flags,
                  FsOpenReply* out) const {
        FsOpenRequest req{};
        req.parent_inode = parent_inode;
        req.flags = flags;
        copy_inline_name(req.name, sizeof(req.name), name, &req.name_len);
        if (!ep.try_send(FS_OPEN, &req, sizeof(req))) return false;
        return wait_open_reply(out);
    }

    bool wait_open(mx_u32_t parent_inode, const char* name, mx_u32_t flags,
                   FsOpenReply* out) const {
        FsOpenRequest req{};
        req.parent_inode = parent_inode;
        req.flags = flags;
        copy_inline_name(req.name, sizeof(req.name), name, &req.name_len);
        if (!ep.send_spinning(FS_OPEN, &req, sizeof(req))) return false;
        return wait_open_reply(out);
    }

    bool try_close(fs_handle_t handle) const {
        return ep.try_send(FS_CLOSE, &handle, sizeof(handle));
    }

    bool try_read(fs_handle_t handle, mx_u64_t offset, mx_u32_t length,
                  FsReadReply* out) const {
        FsReadRequest req{};
        req.handle = handle;
        req.offset = offset;
        req.length = length;
        if (!ep.try_send(FS_READ, &req, sizeof(req))) return false;
        return wait_read_reply(out);
    }

    bool try_write(fs_handle_t handle, mx_u64_t offset, const void* data,
                   mx_u32_t length, FsWriteReply* out) const {
        FsWriteRequest req{};
        req.handle = handle;
        req.offset = offset;
        req.length = length;
        if (length > FS_INLINE_WRITE_MAX) return false;
        if (data && length != 0) copy_bytes(req.payload, data, length);
        if (!ep.try_send(FS_WRITE, &req, sizeof(req))) return false;
        return wait_write_reply(out);
    }

    bool try_promote(fs_handle_t handle, FsPromoteReply* out) const {
        FsPromoteRequest req{handle, 0};
        if (!ep.try_send(FS_PROMOTE, &req, sizeof(req))) return false;
        return wait_promote_reply(out);
    }

    bool try_snapshot(mx_u32_t flags, const char* label, FsSnapshotReply* out) const {
        FsSnapshotRequest req{};
        req.flags = flags;
        const mx_u32_t n = bounded_strlen(label, sizeof(req.label));
        for (mx_u32_t i = 0; i < n; ++i) req.label[i] = static_cast<mx_u8_t>(label[i]);
        if (!ep.try_send(FS_SNAPSHOT, &req, sizeof(req))) return false;
        return wait_snapshot_reply(out);
    }

    bool try_snapshot_mount(mx_u64_t snapshot_id, mx_u32_t flags,
                            FsSnapshotMntReply* out) const {
        FsSnapshotMntRequest req{};
        req.snapshot_id = snapshot_id;
        req.flags = flags;
        if (!ep.try_send(FS_SNAPSHOT_MNT, &req, sizeof(req))) return false;
        return wait_snapshot_mount_reply(out);
    }

    bool try_prefetch(fs_handle_t handle, mx_u32_t type,
                      mx_u64_t offset, mx_u32_t length) const {
        FsPrefetchHint hint{};
        hint.handle = handle;
        hint.type = type;
        hint.offset = offset;
        hint.length = length;
        return ep.try_send(FS_PREFETCH, &hint, sizeof(hint));
    }

    bool try_sync(mx_i32_t* out_status) const {
        if (!ep.try_send(FS_SYNC, nullptr, 0)) return false;
        return wait_status_reply(FS_SYNC_REPLY, out_status);
    }

    bool wait_open_reply(FsOpenReply* out) const {
        return wait_reply(FS_OPEN_REPLY, out);
    }

    bool wait_read_reply(FsReadReply* out) const {
        return wait_reply(FS_READ_REPLY, out);
    }

    bool wait_write_reply(FsWriteReply* out) const {
        return wait_reply(FS_WRITE_REPLY, out);
    }

    bool wait_promote_reply(FsPromoteReply* out) const {
        return wait_reply(FS_PROMOTE_REPLY, out);
    }

    bool wait_snapshot_reply(FsSnapshotReply* out) const {
        return wait_reply(FS_SNAPSHOT_REPLY, out);
    }

    bool wait_snapshot_mount_reply(FsSnapshotMntReply* out) const {
        return wait_reply(FS_SNAPSHOT_MNT_REPLY, out);
    }

    bool wait_status_reply(mx_u32_t type, mx_i32_t* out) const {
        RingSlot slot{};
        while (ep.wait_recv(&slot)) {
            if (slot.type == type && slot.length >= sizeof(mx_i32_t)) {
                mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL);
                copy_bytes(&status, slot.payload, sizeof(status));
                if (out) *out = status;
                return true;
            }
            if (slot.type == FS_ERROR && slot.length >= sizeof(mx_i32_t)) {
                mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL);
                copy_bytes(&status, slot.payload, sizeof(status));
                if (out) *out = status;
                return true;
            }
        }
        return false;
    }

    template <typename T>
    bool wait_reply(mx_u32_t type, T* out) const {
        RingSlot slot{};
        while (ep.wait_recv(&slot)) {
            if (slot.type == type && payload_from_slot(slot, out)) return true;
            if (slot.type == FS_ERROR && slot.length >= sizeof(mx_i32_t)) {
                mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL);
                copy_bytes(&status, slot.payload, sizeof(status));
                if (out) {
                    for (mx_u32_t i = 0; i < sizeof(T); ++i) {
                        reinterpret_cast<mx_u8_t*>(out)[i] = 0;
                    }
                    out->status = status;
                }
                return true;
            }
        }
        return false;
    }
};

struct Server {
    SpscEndpoint ep;
    // Absolute nanosecond deadline for delivering a reply to the peer of this
    // server instance. 0 keeps the legacy unbounded spinning behaviour; the
    // io.fs/nameservice loops pass now + kSessionReplyTimeoutNs so a stalled
    // client cannot hold the service thread (see mochi/sessions.h).
    mx_u64_t reply_deadline_ns = 0;
    // Cleared once a reply could not be delivered. Every later send for this
    // instance is a no-op, so a handler that faults part way through cannot
    // keep hammering a dead ring.
    mutable bool reply_ok = true;

    bool valid() const {
        return ep.valid();
    }

    bool try_recv(RingSlot* out) const {
        return ep.try_recv(out);
    }

    bool wait_recv(RingSlot* out) const {
        return ep.wait_recv(out);
    }

    bool send_payload(const RingSlot& req, mx_u32_t type,
                      const void* payload, mx_u32_t len) const {
        (void)req;
        if (!reply_ok) return false;
        reply_ok = (reply_deadline_ns != 0)
                       ? ep.send_bounded(type, payload, len, reply_deadline_ns)
                       : ep.send_spinning(type, payload, len);
        return reply_ok;
    }

    bool send_error(const RingSlot& req, mx_i32_t status) const {
        return send_payload(req, FS_ERROR, &status, sizeof(status));
    }

    template <typename T>
    bool send_reply(const RingSlot& req, mx_u32_t type, const T& reply) const {
        return send_payload(req, type, &reply, sizeof(reply));
    }
};

struct BlkClient {
    SpscEndpoint ep;

    bool valid() const {
        return ep.valid();
    }

    bool request(mx_u32_t type, mx_u64_t sector, mx_u32_t sector_count,
                 mx_handle_t arena_cap, mx_u32_t arena_offset,
                 BlkReply* out, mx_u64_t deadline_ns = 0) const {
        BlkRequest req{};
        req.type = type;
        req.sector = sector;
        req.sector_count = sector_count;
        req.arena_cap = arena_cap;
        req.arena_offset = arena_offset;
        if (!ep.send_spinning(type, &req, sizeof(req))) return false;
        RingSlot slot{};
        for (;;) {
            if (ep.try_recv(&slot)) {
                if (slot.type == BLK_REPLY && payload_from_slot(slot, out)) return true;
                continue;
            }
            if (deadline_ns == 0 || mx_clock_ns() >= deadline_ns) return false;
            sys_yield();
        }
    }
};

struct BlkServer {
    SpscEndpoint ep;

    bool valid() const {
        return ep.valid();
    }

    bool try_recv(RingSlot* out) const {
        return ep.try_recv(out);
    }

    bool wait_recv(RingSlot* out) const {
        return ep.wait_recv(out);
    }

    bool send_reply(mx_i32_t status, mx_u32_t sectors_transferred) const {
        BlkReply reply{status, sectors_transferred};
        return ep.send_spinning(BLK_REPLY, &reply, sizeof(reply));
    }
};

} // namespace mochi::fs

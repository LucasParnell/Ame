#pragma once
// mochi/fs.h — small helpers for the io.fs and drv.blk.virtio ring protocols.

#include "libk/bytes.h"
#include "libk/str.h"
#include "mochi/endpoint.h"
#include "midix/abi/protocols/fs.h"

namespace mochi::fs {

// Inline names reserve one byte for NUL; the wire length excludes it.
template <mx_size_t N>
static inline mx_u8_t copy_inline_name(char (&dst)[N], const char* src) {
    static_assert(N - 1 <= 0xFF, "inline name length must fit its wire field");
    return static_cast<mx_u8_t>(libk::str::copy_into(dst, N, src));
}

struct Client {
    SpscEndpoint ep;

    bool valid() const {
        return ep.valid();
    }

    bool try_open(mx_u32_t parent_inode, const char* name, mx_u32_t flags,
                  FsOpenReply* out) const {
        FsOpenRequest req{};
        if (!name || libk::str::length_n(name, sizeof(req.name)) >= sizeof(req.name)) return false;
        req.parent_inode = parent_inode;
        req.flags = flags;
        req.name_len = copy_inline_name(req.name, name);
        if (!ep.try_send(FS_OPEN, &req, sizeof(req))) return false;
        return wait_open_reply(out);
    }

    bool wait_open(mx_u32_t parent_inode, const char* name, mx_u32_t flags,
                   FsOpenReply* out) const {
        FsOpenRequest req{};
        if (!name || libk::str::length_n(name, sizeof(req.name)) >= sizeof(req.name)) return false;
        req.parent_inode = parent_inode;
        req.flags = flags;
        req.name_len = copy_inline_name(req.name, name);
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
        if (length > FS_INLINE_WRITE_MAX || (!data && length)) return false;
        if (data) (void)libk::copy_n(libk::as_bytes(req.payload), libk::as_bytes(data, length), length);
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
        const auto n = libk::str::length_n(label, sizeof(req.label));
        (void)libk::copy_n(libk::as_bytes(req.label), libk::as_bytes(label, n), n);
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
        return wait_reply(FS_READ_REPLY, out) && out->length_read <= sizeof(out->inline_data);
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
            mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL);
            if ((slot.type == type || slot.type == FS_ERROR) && message_get(slot, &status)) {
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
            if (message_get(slot, out, type)) return true;
            mx_i32_t status = static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL);
            if (message_get(slot, &status, FS_ERROR)) {
                if (out) {
                    libk::zero(*out);
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
        return send_payload(req, type, &reply, message_size<T>());
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
        if (!out) return false;
        const bool sent = deadline_ns
            ? ep.send_bounded(type, &req, sizeof(req), deadline_ns)
            : ep.send_spinning(type, &req, sizeof(req));
        if (!sent) return false;
        RingSlot slot{};
        for (;;) {
            if (ep.try_recv(&slot)) {
                if (message_get(slot, out, BLK_REPLY)) return true;
                if (deadline_ns != 0 && mx_clock_ns() >= deadline_ns) return false;
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

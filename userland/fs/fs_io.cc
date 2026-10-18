// Bounded per-session arena transport and immutable client read views.
#include "fs_internal.h"
#include "libk/endian.h"
#include "midix/abi/session.h"

FsTransfer g_transfers[MX_MAX_SESSIONS]{};
namespace {
constexpr mx_u32_t kReadViews = 16, kViewsPerClient = 2;
#ifndef MOCHI_FS_READ_BASE
#define MOCHI_FS_READ_BASE 0x2d000000ull
#endif
const mx_u64_t kReadBase = MOCHI_FS_READ_BASE;
// Search hot identities separately; the actual pages are ordinary coherent
// arenas mapped in a compact, fixed window only when a cache slot is needed.
mx_u32_t owners[kReadViews], inodes[kReadViews], valid_bytes[kReadViews];
mx_u64_t offsets[kReadViews], generations[kReadViews];
mx_handle_t caps[kReadViews];
mx_u64_t address(mx_u32_t i) { return kReadBase + i * FS_READ_ARENA_MAX; }
mx_u8_t *bytes(mx_u32_t i) { return reinterpret_cast<mx_u8_t *>(address(i)); }
bool reusable(mx_u32_t i) {
    if (!caps[i])
        return true;
    MxCapInfo info{};
    return mx_cap_info(caps[i], &info) == MX_OK && (info.flags >> MX_ARENA_REFERENCE_SHIFT) == 2;
}
} // namespace

void close_transfer_session(mx_u32_t session) {
    if (session >= MX_MAX_SESSIONS)
        return;
    g_transfers[session] = {};
    for (mx_u32_t i = 0; i < kReadViews; ++i) {
        if (!caps[i] || owners[i] != session)
            continue;
        (void)sys_unmap_page(address(i));
        (void)sys_cap_revoke(caps[i]);
        // Client mappings, if any, retain the old object. A later session gets
        // a fresh arena; it never inherits the preceding generation's bytes.
        caps[i] = 0;
        valid_bytes[i] = 0;
    }
}
mx_i32_t volume_read(const OpenHandle &handle, const FsReadRequest &req, FsReadReply &reply) {
    if (req.reserved || (req.flags & ~(FS_READ_ZERO_COPY | FS_READ_DIRECT)))
        return MX_ERR_INVALID;
    if (req.flags & FS_READ_DIRECT)
        return MX_ERR_NOT_IMPLEMENTED;
    if (!(handle.access & FS_ACCESS_READ))
        return MX_ERR_PERM;
    if (!req.length || (!(req.flags & FS_READ_ZERO_COPY) && req.length <= FS_INLINE_READ_MAX))
        return mochi::store::read(g_volume, handle.inode_number, req.offset, reply.inline_data, req.length,
                                  reply.length_read);
    if (g_session >= MX_MAX_SESSIONS || !g_transfers[g_session].peer)
        return MX_ERR_NOT_IMPLEMENTED;
    const auto generation = g_volume.checkpoint.current_generation;
    mx_u32_t selected = kReadViews, owned = 0;
    for (mx_u32_t i = 0; i < kReadViews; ++i) {
        if (caps[i] && owners[i] == g_session) {
            ++owned;
            if (inodes[i] == handle.inode_number && offsets[i] == req.offset &&
                generations[i] == generation && valid_bytes[i]) {
                selected = i;
                break;
            }
        }
    }
    const bool hit = selected != kReadViews;
    if (!hit) {
        for (mx_u32_t i = 0; i < kReadViews; ++i) {
            if ((caps[i] && owners[i] == g_session) || owned < kViewsPerClient) {
                if (reusable(i)) {
                    selected = i;
                    break;
                }
            }
        }
        if (selected == kReadViews)
            return MX_ERR_BUSY;
        if (!caps[selected]) {
            mx_handle_t cap = 0;
            if (mx_arena_create(FS_READ_ARENA_MAX, 0, &cap) != MX_OK)
                return MX_ERR_NOMEM;
            if (mx_map_page(cap, address(selected), MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
                (void)sys_cap_revoke(cap);
                return MX_ERR_NOMEM;
            }
            caps[selected] = cap;
        }
        valid_bytes[selected] = 0;
        libk::zero(bytes(selected), FS_READ_ARENA_MAX);
        const auto length = FS_READ_ARENA_MAX - static_cast<mx_u32_t>(req.offset % FS_READ_ARENA_MAX);
        mx_u32_t received = 0;
        const auto status =
            mochi::store::read(g_volume, handle.inode_number, req.offset, bytes(selected), length, received);
        if (status != MX_OK)
            return status;
        if (!received) {
            reply.length_read = 0;
            return MX_OK;
        }
        owners[selected] = g_session;
        inodes[selected] = handle.inode_number;
        offsets[selected] = req.offset;
        generations[selected] = generation;
        valid_bytes[selected] = received;
    }
    mx_handle_t client_cap = 0;
    const auto result =
        mx_cap_grant(g_transfers[g_session].peer, caps[selected], MX_RIGHT_READ | MX_RIGHT_MAP, &client_cap);
    if (result != MX_OK)
        return result;
    reply.arena_cap = client_cap;
    reply.length_read = req.length < valid_bytes[selected] ? req.length : valid_bytes[selected];
    return MX_OK;
}
mx_i32_t volume_write(const OpenHandle &handle, const FsWriteRequest &req) {
    if (!(handle.access & FS_ACCESS_WRITE))
        return MX_ERR_PERM;
    if (req.reserved || (req.flags & ~FS_WRITE_ZERO_COPY))
        return MX_ERR_INVALID;
    if (!(req.flags & FS_WRITE_ZERO_COPY)) {
        if (req.length > FS_INLINE_WRITE_MAX)
            return MX_ERR_INVALID;
        return mochi::store::write(g_volume, handle.inode_number, req.offset, req.payload, req.length);
    }
    if (g_session >= MX_MAX_SESSIONS)
        return MX_ERR_INVALID;
    const auto &transfer = g_transfers[g_session];
    const auto token = libk::load_le<mx_u64_t>(req.payload);
    const auto offset = libk::load_le<mx_u64_t>(req.payload + 8);
    if (!transfer.write || !transfer.write_token || token != transfer.write_token ||
        offset > FS_TRANSFER_MAX || req.length > FS_TRANSFER_MAX - offset ||
        libk::load_le<mx_u64_t>(req.payload + 16))
        return MX_ERR_INVALID;
    return mochi::store::write(g_volume, handle.inode_number, req.offset, transfer.write + offset,
                               req.length);
}

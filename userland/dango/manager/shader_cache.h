#pragma once
#include "mochi/fs.h"
#include "midix/abi/protocols/graphics_cache.h"

// The VM receives opaque bounded bytes, never a path or filesystem handle.
// Only the manager's control thread calls this; its vCPU runner stays separate.
struct ShaderCache {
    mochi::fs::Client fs;
    mx_u8_t* transfer{};
    fs_handle_t directory{}, file{};
    mx_u32_t cursor{}, total{}, operation{};
    mx_u64_t sequence{};
    void initialize(const MxBootInfo* boot) {
        const auto cap = mochi::boot::handle(boot, "gfx.cache");
        if (!cap || mx_map_page(cap, 0x34000000, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
            return;
        }
        transfer = reinterpret_cast<mx_u8_t*>(0x34000000);
        fs.ep = mochi::boot::map_endpoint(boot, {"fs.cmd.ring", 0x34100000, "fs.cmd.doorbell"},
                                          {"fs.reply.ring", 0x34110000, "fs.reply.doorbell"});
        if (fs.valid()) {
            (void)fs.map_write_arena(boot, 0x34200000);
        }
    }
    void close() {
        if (file) {
            mx_i32_t ignored{};
            if (fs.try_close(file)) {
                (void)fs.wait_status_reply(FS_CLOSE_REPLY, &ignored);
            }
        }
        file = 0;
        cursor = total = operation = 0;
    }
    ame_gfx_cache_io exchange(const ame_gfx_cache_io& request) {
        auto reply = request;
        reply.status = -1;
        reply.size = reply.total = 0;
        if (!fs.valid() || !fs.write_buffer || !transfer || request.reserved || request.status ||
            !request.id || request.id <= sequence ||
            (request.operation != AME_GFX_CACHE_LOAD && request.operation != AME_GFX_CACHE_SAVE) ||
            !request.size || request.size > AME_GFX_CACHE_TRANSFER ||
            request.offset > AME_GFX_CACHE_MAX || request.total > AME_GFX_CACHE_MAX) {
            return reply;
        }
        sequence = request.id;
        if (!request.offset) {
            close();
            FsOpenReply opened{};
            if (!directory) {
                if (!fs.try_open(1, "data", FS_OPEN_READ, &opened) || opened.status != MX_OK) {
                    return reply;
                }
                directory = opened.handle;
            }
            const bool save = request.operation == AME_GFX_CACHE_SAVE;
            if (save && (!request.total || request.size > request.total)) {
                return reply;
            }
            if (!fs.try_open(directory, "mesa-blobs-v1",
                             save ? FS_OPEN_WRITE | FS_OPEN_CREATE | FS_OPEN_TRUNC : FS_OPEN_READ,
                             &opened) ||
                opened.status != MX_OK) {
                return reply;
            }
            file = opened.handle;
            operation = request.operation;
            const auto length = save ? request.total : opened.size;
            if (length > AME_GFX_CACHE_MAX || !length) {
                close();
                return reply;
            }
            total = static_cast<mx_u32_t>(length);
        }
        if (!file || operation != request.operation || request.offset != cursor ||
            cursor >= total ||
            (operation == AME_GFX_CACHE_SAVE &&
             (request.total != total || request.size > total - cursor))) {
            close();
            return reply;
        }
        mx_u32_t count = request.size > total - cursor ? total - cursor : request.size;
        bool ok;
        if (operation == AME_GFX_CACHE_LOAD) {
            mx_u32_t received{};
            ok = fs.read_bytes(file, cursor, transfer, count, &received, 0x34300000) &&
                 received == count;
        } else {
            FsWriteReply written{};
            ok = fs.try_write(file, cursor, transfer, count, &written) && written.status == MX_OK &&
                 written.bytes_written == count;
        }
        if (!ok) {
            close();
            return reply;
        }
        cursor += count;
        reply.size = count;
        reply.total = total;
        reply.status = 0;
        if (cursor == total) {
            const bool save = operation == AME_GFX_CACHE_SAVE;
            close();
            if (save) {
                mx_i32_t status{};
                if (!fs.try_sync(&status) || status != MX_OK) {
                    reply.status = -1;
                }
            }
        }
        return reply;
    }
};

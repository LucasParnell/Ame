#pragma once
/* C/C++ command decoding shared by Vulkan executors and command producers.
 * Decode from a private snapshot: the caller must not validate shared bytes
 * and then reread them for execution. Operands never address command storage. */
#include "midix/abi/protocols/vulkan_session.h"

typedef union MxVkCommand {
    MxVkCommandHeader header;
    MxVkCmdFillBuffer fill;
    MxVkCmdCopyBuffer copy;
    MxVkCmdDispatch dispatch;
    MxVkCmdBindVertexBuffer vertex;
    MxVkCmdBindRenderTarget target;
    MxVkCmdDraw draw;
} MxVkCommand;

static inline int mx_vk_data_range_valid(mx_u64_t offset, mx_u64_t size, mx_u32_t arena_bytes) {
    return size != 0 && offset >= MX_VK_COMMAND_REGION_BYTES && offset <= arena_bytes &&
           size <= arena_bytes - offset;
}

/* Copies unaligned records safely, checks their exact ABI size and reserved
 * fields. The caller advances by out->header.size only on success. */
static inline int mx_vk_command_decode(const mx_u8_t* bytes, mx_u32_t available, MxVkCommand* out) {
    if (!bytes || !out || available < sizeof(MxVkCommandHeader)) {
        return -1;
    }
    mx_u8_t* dst = (mx_u8_t*)out;
    for (mx_u32_t i = 0; i < sizeof(*out); ++i) {
        dst[i] = 0;
    }
    for (mx_u32_t i = 0; i < sizeof(MxVkCommandHeader); ++i) {
        dst[i] = bytes[i];
    }
    mx_u32_t expected;
    switch (out->header.opcode) {
    case VK_CMD_PIPELINE_BARRIER:
        expected = sizeof(MxVkCommandHeader);
        break;
    case VK_CMD_FILL_BUFFER:
        expected = sizeof(MxVkCmdFillBuffer);
        break;
    case VK_CMD_COPY_BUFFER:
        expected = sizeof(MxVkCmdCopyBuffer);
        break;
    case VK_CMD_DISPATCH:
        expected = sizeof(MxVkCmdDispatch);
        break;
    case VK_CMD_BIND_VERTEX_BUFFER:
        expected = sizeof(MxVkCmdBindVertexBuffer);
        break;
    case VK_CMD_BIND_RENDER_TARGET:
        expected = sizeof(MxVkCmdBindRenderTarget);
        break;
    case VK_CMD_DRAW:
        expected = sizeof(MxVkCmdDraw);
        break;
    default:
        return -1;
    }
    if (out->header.flags || out->header.size != expected || expected > available) {
        return -1;
    }
    for (mx_u32_t i = sizeof(MxVkCommandHeader); i < expected; ++i) {
        dst[i] = bytes[i];
    }
    if ((out->header.opcode == VK_CMD_FILL_BUFFER && out->fill.reserved0) ||
        (out->header.opcode == VK_CMD_DISPATCH && out->dispatch.reserved0) ||
        (out->header.opcode == VK_CMD_BIND_RENDER_TARGET && out->target.reserved0)) {
        return -1;
    }
    return 0;
}

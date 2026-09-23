#pragma once
// Midix block-driver protocol ABI.
//
// The block protocol is deliberately separate from the filesystem protocol: a
// block device (virtio-blk today) serves raw sectors and knows nothing about
// inodes, extents or snapshots. Keeping MX_PROTOCOL_BLK and its wire records in
// their own header makes that boundary explicit instead of burying them inside
// protocols/fs.h.
//
// All request/reply payload structs below are sized for MxRingSlot::payload,
// which is 48 bytes on the V0 64-byte control-ring slot.

#include "midix/abi/types.h"
#include "midix/abi/status.h"
#include "midix/abi/ring.h"

constexpr mx_u32_t MX_PROTOCOL_BLK = 0x424C4B00u; // 'BLK\0'

using mx_cap_handle_t = mx_handle_t;

enum BlkMsgType : mx_u32_t {
    BLK_READ  = 1,
    BLK_WRITE = 2,
    BLK_FLUSH = 3,
    BLK_REPLY = 4,
};

// Offset inside a client arena from which the device may DMA.
constexpr mx_u32_t BLK_DMA_CLIENT_OFFSET = 0x0001'0000u;

// The control-ring frame for this protocol is the shared ring slot verbatim.
using MxBlkFrame = MxRingSlot;
static_assert(sizeof(MxBlkFrame) == 64, "block frame must be one ring slot");

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

/*
 * gpud_protocol.c — object-id codec and control-frame helpers for dango-gpud.
 *
 * These are transport mechanics only: no DRM, no device state. The wire layout
 * they operate on is the byte-exact projection in gpud_protocol.h.
 */
#include "gpud_protocol.h"

uint64_t gpud_object_id(uint64_t generation, uint32_t type, uint32_t index) {
    return ((generation << GPUD_OBJ_GEN_SHIFT) & GPUD_OBJ_GEN_MASK) |
           (((uint64_t)type << GPUD_OBJ_TYPE_SHIFT) & GPUD_OBJ_TYPE_MASK) |
           ((uint64_t)index & GPUD_OBJ_INDEX_MASK);
}

int gpud_object_index(uint64_t id, uint32_t type, uint64_t generation, uint32_t *index_out) {
    uint32_t index;
    if (id == 0) {
        return -1;
    }
    index = (uint32_t)(id & GPUD_OBJ_INDEX_MASK);
    if (index == 0) {
        return -1;
    }
    if ((id & GPUD_OBJ_GEN_MASK) !=
        ((generation << GPUD_OBJ_GEN_SHIFT) & GPUD_OBJ_GEN_MASK)) {
        return -1;
    }
    if (((uint32_t)((id & GPUD_OBJ_TYPE_MASK) >> GPUD_OBJ_TYPE_SHIFT)) != (type & 0xFu)) {
        return -1;
    }
    if (index_out) {
        *index_out = index;
    }
    return 0;
}

int gpud_opcode_supported(uint32_t opcode) {
    switch (opcode) {
    case GPU_HELLO:
    case GPU_ENUM_OUTPUTS:
    case GPU_GET_MODES:
    case GPU_BUFFER_REGISTER:
    case GPU_BUFFER_UNREGISTER:
    case GPU_SET_MODE:
    case GPU_PRESENT:
        return 1;
    default:
        return 0;
    }
}

int gpud_frame_decode(const struct dango_ring_slot *slot, uint32_t expected_opcode,
                      uint32_t expected_bytes, const void **payload) {
    if (!slot || slot->type != expected_opcode) {
        return -1;
    }
    /* Every request payload is a fixed inline frame; a different length is a
     * malformed client, not a shortened form to be guessed at. */
    if (slot->length != expected_bytes || expected_bytes > DANGO_RING_PAYLOAD_BYTES) {
        return -1;
    }
    if (payload) {
        *payload = slot->payload;
    }
    return 0;
}

void gpud_frame_reply(struct dango_ring_slot *slot, uint32_t opcode, uint64_t seq,
                      const void *payload, uint32_t bytes) {
    const unsigned char *src = (const unsigned char *)payload;
    uint32_t i;
    if (bytes > DANGO_RING_PAYLOAD_BYTES) {
        bytes = DANGO_RING_PAYLOAD_BYTES;
    }
    slot->type = opcode;
    slot->length = bytes;
    slot->seq = seq;
    /* Zero the tail so a reply never leaks a previous frame's bytes. */
    for (i = 0; i < DANGO_RING_PAYLOAD_BYTES; ++i) {
        slot->payload[i] = 0;
    }
    for (i = 0; i < bytes; ++i) {
        slot->payload[i] = src[i];
    }
}

void gpud_frame_error(struct dango_ring_slot *slot, const struct dango_ring_slot *request,
                      int64_t status, uint64_t object_id, uint64_t generation, uint64_t detail) {
    struct MxGpuError error;
    error.status = status;
    error.object_id = object_id;
    error.boot_generation = generation;
    error.detail = detail;
    error.opcode = request ? request->type : 0;
    error.flags = 0;
    error.reserved0 = 0;
    error.reserved1 = 0;
    gpud_frame_reply(slot, GPU_ERROR, request ? request->seq : 0, &error,
                     (uint32_t)sizeof(error));
}

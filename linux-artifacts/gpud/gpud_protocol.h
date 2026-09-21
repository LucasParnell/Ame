#pragma once
/*
 * gpud_protocol.h — gpud-internal protocol helpers (object identity, capacity).
 *
 * The wire contract itself is `midix/abi/protocols/gpu.h`, which is C/C++
 * compatible (C gets the Dango channel ring types), so this header simply
 * includes it: there is one definition of every opcode, struct and flag, and no
 * C projection to keep in sync.
 */
#ifndef MIDIX_GPUD_PROTOCOL_H
#define MIDIX_GPUD_PROTOCOL_H

#include <stdint.h>

#include "midix/abi/protocols/gpu.h"

/* -------------------------------------------------------------------------- */
/* Object identity + control-frame helpers (gpud-internal, not wire types).   */
/*                                                                            */
/* Remote object ids are opaque 64-bit values scoped to one boot generation:  */
/*   [ generation:48 ][ type:4 ][ index:12 ]                                  */
/* `dango-gpud` always validates the generation and type before it trusts an  */
/* index, so an id minted by an earlier appliance boot, or one used as the    */
/* wrong object kind, fails validation instead of aliasing a live object.     */
/* -------------------------------------------------------------------------- */
#define GPUD_OBJ_INDEX_BITS 12
#define GPUD_OBJ_INDEX_MASK 0x0000000000000FFFull
#define GPUD_OBJ_TYPE_SHIFT 12
#define GPUD_OBJ_TYPE_MASK 0x000000000000F000ull
#define GPUD_OBJ_GEN_SHIFT 16
#define GPUD_OBJ_GEN_MASK 0xFFFFFFFFFFFF0000ull

/* Fixed capacities: no hot-path allocation anywhere in the server. */
#define GPUD_MAX_OUTPUTS 8
#define GPUD_MAX_MODES 32
#define GPUD_MAX_BUFFERS 8
#define GPUD_MAX_PENDING 4

/* Mint the id for object `index` (1-based) of `type` in `generation`. */
uint64_t gpud_object_id(uint64_t generation, uint32_t type, uint32_t index);

/* Decode `id` and confirm it names `type` in `generation`. Returns 0 and stores
 * the 1-based index when valid, or -1 when the id is stale, mistyped or null. */
int gpud_object_index(uint64_t id, uint32_t type, uint64_t generation, uint32_t *index_out);

/* 1 when the opcode is one of the bootstrap opcodes this server implements. */
int gpud_opcode_supported(uint32_t opcode);

/* Validate `slot` as a request of `expected_opcode` whose inline payload is
 * exactly `expected_bytes`, then point `*payload` at it. Returns 0 on success,
 * -1 when the opcode or length does not match. */
int gpud_frame_decode(const struct dango_ring_slot *slot, uint32_t expected_opcode,
                      uint32_t expected_bytes, const void **payload);

/* Build an opcode reply frame: opcode, echoed seq, inline payload. */
void gpud_frame_reply(struct dango_ring_slot *slot, uint32_t opcode, uint64_t seq,
                      const void *payload, uint32_t bytes);

/* Build a GPU_ERROR frame answering `request`. */
void gpud_frame_error(struct dango_ring_slot *slot, const struct dango_ring_slot *request,
                      int64_t status, uint64_t object_id, uint64_t generation, uint64_t detail);

#endif /* MIDIX_GPUD_PROTOCOL_H */

#pragma once
/*
 * gpud_protocol.h — the Midix GPU protocol (spec §13) as consumed by the
 * freestanding C appliance userspace (`dango-gpud`).
 *
 * `midix/abi/protocols/gpu.h` is the frozen authority for this wire contract,
 * but it is a native-C++ header (`<cstdint>`, `constexpr`, `using`, fixed
 * underlying `enum : T`) and cannot be included from freestanding C. What
 * follows is therefore the byte-exact C projection of that header: the same
 * opcode values, struct sizes, field offsets and feature/flag values, asserted
 * below. It is not an independent protocol definition — every number is copied
 * from `protocols/gpu.h` and is cross-checked by the host regression
 * `tests/gpu_proto_probe.cc` against the very same values, so drift is a build
 * or test failure, never a silent wire mismatch.
 *
 * The projection is compiled only for C; a native-C++ translation unit (or a
 * build that defines GPUD_PROTOCOL_USE_FROZEN_HEADER) includes the frozen
 * header directly instead, with no redefinition anywhere.
 */
#ifndef MIDIX_GPUD_PROTOCOL_H
#define MIDIX_GPUD_PROTOCOL_H

#include <stdint.h>

#include "midix/abi/dango_channel_guest.h"

#if defined(__cplusplus) || defined(GPUD_PROTOCOL_USE_FROZEN_HEADER)

#include "midix/abi/protocols/gpu.h"

#else /* !__cplusplus && !GPUD_PROTOCOL_USE_FROZEN_HEADER */

/* --- Protocol identity (protocols/gpu.h) --------------------------------- */
#define MX_PROTOCOL_GPU 0x47505500u /* 'GPU\0' */
#define MX_GPU_VERSION_MAJOR 1
#define MX_GPU_VERSION_MINOR 0

/* --- Status codes (midix/abi/status.h) ----------------------------------- */
enum {
    MX_OK = 0,
    MX_ERR_INVALID = -1,
    MX_ERR_PERM = -2,
    MX_ERR_NOMEM = -3,
    MX_ERR_NOT_FOUND = -4,
    MX_ERR_BAD_TYPE = -5,
    MX_ERR_OVERFLOW = -6,
    MX_ERR_DEAD = -7,
    MX_ERR_BUSY = -8,
    MX_ERR_FAULT = -9,
    MX_ERR_NOT_IMPLEMENTED = -10,
    MX_ERR_SERVICE_UNAVAILABLE = -11,
    MX_ERR_BAD_PROTOCOL = -12,
    MX_ERR_WOULD_BLOCK = -13,
    MX_ERR_BAD_IMAGE = -14,
    MX_ERR_BAD_SIGNATURE = -15,
    MX_ERR_BAD_CHECKSUM = -16,
    MX_ERR_POLICY = -17,
};

/* --- Opcodes ------------------------------------------------------------- */
enum MxGpuOpcode {
    GPU_HELLO = 0x47505501u,
    GPU_ENUM_OUTPUTS = 0x47505502u,
    GPU_GET_MODES = 0x47505503u,
    GPU_BUFFER_REGISTER = 0x47505504u,
    GPU_BUFFER_UNREGISTER = 0x47505505u,
    GPU_SET_MODE = 0x47505506u,
    GPU_PRESENT = 0x47505507u,
    GPU_PRESENT_COMPLETE = 0x47505508u,
    GPU_ERROR = 0x47505509u,
};

/* The reserved future block. `dango-gpud` never accepts one of these in this
 * bootstrap profile; the values exist so a client can tell a future opcode
 * apart from garbage. */
#define MX_GPU_OPCODE_FUTURE_BASE 0x47505580u
#define MX_GPU_OPCODE_FUTURE_LIMIT 0x475055FFu

/* --- Object types -------------------------------------------------------- */
enum MxGpuObjectType {
    MX_GPU_OBJECT_DEVICE = 1,
    MX_GPU_OBJECT_OUTPUT = 2,
    MX_GPU_OBJECT_MODE = 3,
    MX_GPU_OBJECT_BUFFER = 4,
    MX_GPU_OBJECT_FRAMEBUFFER = 5,
    MX_GPU_OBJECT_CONTEXT = 6,
    MX_GPU_OBJECT_FENCE = 7,
};

/* --- Feature bits -------------------------------------------------------- */
#define MX_GPU_FEATURE_STAGING_BUFFER (1ull << 0)
#define MX_GPU_FEATURE_ASYNC_PRESENT (1ull << 1)
#define MX_GPU_FEATURE_DAMAGE_RECTS (1ull << 2)
#define MX_GPU_FEATURE_CURSOR (1ull << 3)
#define MX_GPU_FEATURE_ATOMIC_COMMIT (1ull << 4)
#define MX_GPU_FEATURE_BUFFER_EXPORT (1ull << 5)
#define MX_GPU_FEATURE_RENDER_CONTEXT (1ull << 6)
#define MX_GPU_FEATURE_REMOTE_VULKAN (1ull << 7)

/* --- Flags --------------------------------------------------------------- */
#define MX_GPU_HELLO_FLAG_NONE 0u

#define MX_GPU_PRESENT_FLAG_NONE 0u
#define MX_GPU_PRESENT_FLAG_FULL_FRAME (1u << 0)
#define MX_GPU_PRESENT_FLAG_ASYNC (1u << 1)

#define MX_GPU_BUFFER_FLAG_NONE 0u
#define MX_GPU_BUFFER_FLAG_STAGING (1u << 0)
#define MX_GPU_BUFFER_FLAG_SCANOUT (1u << 1)

/* Pixel formats are Midix-native; XRGB8888 matches the bootstrap shared
 * staging scanout buffer (spec §12.2). */
#define MX_GPU_PIXFMT_XRGB8888 1u

/* --- Payloads ------------------------------------------------------------ */

struct MxGpuDamageRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

#define MX_GPU_MAX_INLINE_DAMAGE 1u

struct MxGpuHello {
    uint16_t major;
    uint16_t minor;
    uint32_t flags;
    uint64_t feature_bits;
    uint64_t boot_generation;
};

struct MxGpuHelloReply {
    int64_t status;
    uint16_t major;
    uint16_t minor;
    uint32_t flags;
    uint64_t feature_bits;
    uint64_t boot_generation;
};

struct MxGpuEnumOutputsRequest {
    uint64_t device_id;
    uint64_t boot_generation;
    uint32_t index;
    uint32_t flags;
};

struct MxGpuEnumOutputsReply {
    int64_t status;
    uint64_t output_id;
    uint32_t output_count;
    uint32_t output_type;
    uint32_t connection;
    uint32_t width_mm;
    uint32_t height_mm;
    uint32_t flags;
    uint8_t name[8];
};

struct MxGpuGetModesRequest {
    uint64_t output_id;
    uint64_t boot_generation;
    uint32_t index;
    uint32_t flags;
};

struct MxGpuGetModesReply {
    int64_t status;
    uint64_t mode_id;
    uint32_t mode_count;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    uint32_t pixel_format;
    uint32_t flags;
    uint8_t name[8];
};

struct MxGpuBufferRegisterRequest {
    uint64_t shm_id; /* staging offset in bytes within the shared window */
    uint64_t boot_generation;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t reserved0;
};

struct MxGpuBufferRegisterReply {
    int64_t status;
    uint64_t buffer_id;
    uint64_t boot_generation;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct MxGpuBufferUnregisterRequest {
    uint64_t buffer_id;
    uint64_t boot_generation;
};

struct MxGpuBufferUnregisterReply {
    int64_t status;
    uint64_t boot_generation;
};

struct MxGpuSetModeRequest {
    uint64_t output_id;
    uint64_t mode_id;
    uint64_t buffer_id;
    uint64_t boot_generation;
    uint32_t flags;
    uint32_t reserved0;
};

struct MxGpuSetModeReply {
    int64_t status;
    uint64_t framebuffer_id;
    uint64_t boot_generation;
};

struct MxGpuPresentRequest {
    uint64_t buffer_id;
    uint64_t framebuffer_id;
    uint64_t boot_generation;
    uint32_t flags;
    uint32_t damage_count; /* 0 => whole framebuffer */
    struct MxGpuDamageRect damage;
};

struct MxGpuPresentReply {
    int64_t status;
    uint64_t buffer_id;
    uint64_t boot_generation;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct MxGpuPresentComplete {
    int64_t status;
    uint64_t buffer_id;
    uint64_t boot_generation;
    uint32_t flags;
    uint32_t reserved0;
};

struct MxGpuError {
    int64_t status;
    uint64_t object_id;
    uint64_t boot_generation;
    uint64_t detail;
    uint32_t opcode;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
};

/* The control frame is the shared ring slot verbatim. */
typedef struct dango_ring_slot MxGpuFrame;

/* --- Layout drift checks (mirror protocols/gpu.h + the host probe) ------- */
#define GPUD_ASSERT(cond, msg) _Static_assert((cond), msg)

GPUD_ASSERT(sizeof(struct MxGpuDamageRect) == 16, "damage rectangle must be 16 bytes");
GPUD_ASSERT(sizeof(struct MxGpuHello) == 24, "GPU_HELLO size drift");
GPUD_ASSERT(sizeof(struct MxGpuHelloReply) == 32, "GPU_HELLO reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuEnumOutputsRequest) == 24, "GPU_ENUM_OUTPUTS request size drift");
GPUD_ASSERT(sizeof(struct MxGpuEnumOutputsReply) == 48, "GPU_ENUM_OUTPUTS reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuGetModesRequest) == 24, "GPU_GET_MODES request size drift");
GPUD_ASSERT(sizeof(struct MxGpuGetModesReply) == 48, "GPU_GET_MODES reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuBufferRegisterRequest) == 40, "GPU_BUFFER_REGISTER request size drift");
GPUD_ASSERT(sizeof(struct MxGpuBufferRegisterReply) == 32, "GPU_BUFFER_REGISTER reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuBufferUnregisterRequest) == 16,
            "GPU_BUFFER_UNREGISTER request size drift");
GPUD_ASSERT(sizeof(struct MxGpuBufferUnregisterReply) == 16,
            "GPU_BUFFER_UNREGISTER reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuSetModeRequest) == 40, "GPU_SET_MODE request size drift");
GPUD_ASSERT(sizeof(struct MxGpuSetModeReply) == 24, "GPU_SET_MODE reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuPresentRequest) == 48, "GPU_PRESENT request size drift");
GPUD_ASSERT(sizeof(struct MxGpuPresentReply) == 32, "GPU_PRESENT reply size drift");
GPUD_ASSERT(sizeof(struct MxGpuPresentComplete) == 32, "GPU_PRESENT_COMPLETE size drift");
GPUD_ASSERT(sizeof(struct MxGpuError) == 48, "GPU_ERROR size drift");
GPUD_ASSERT(sizeof(MxGpuFrame) == 64, "GPU frame must be one ring slot");
GPUD_ASSERT(sizeof(struct MxGpuHello) <= DANGO_RING_PAYLOAD_BYTES, "GPU_HELLO payload budget");
GPUD_ASSERT(sizeof(struct MxGpuError) <= DANGO_RING_PAYLOAD_BYTES, "GPU_ERROR payload budget");
GPUD_ASSERT(sizeof(struct MxGpuPresentRequest) == 48, "GPU_PRESENT payload budget");

/* Field placements that the host probe pins as offsets. */
GPUD_ASSERT(__builtin_offsetof(struct MxGpuEnumOutputsReply, output_id) == 8, "reply output_id");
GPUD_ASSERT(__builtin_offsetof(struct MxGpuSetModeRequest, boot_generation) == 24, "set_mode gen");
GPUD_ASSERT(__builtin_offsetof(struct MxGpuPresentRequest, damage) == 32, "present damage off");
GPUD_ASSERT(MX_GPU_OPCODE_FUTURE_BASE > GPU_ERROR, "bootstrap opcodes precede the future block");

#undef GPUD_ASSERT

#endif /* C projection of protocols/gpu.h */

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

#pragma once
// Midix GPU protocol ABI (Ame GPU appliance, spec §13).
//
// This header is the stable contract between native Mochi clients,
// `drv.gpu.linux`, and the Linux `dango-gpud` userspace build. It carries no
// Linux DRM UAPI types: object IDs are Midix-opaque 64-bit values scoped to one
// appliance boot generation, and only `dango-gpud` may map them to DRM handles.
//
// Transport envelope: MxRingSlot. `type` is the opcode, `seq` is the
// correlation field, and `length` bounds the inline payload. Every payload is
// sized for the 48-byte inline budget; larger structures live in a referenced
// shared arena (spec §13.2).

#include "midix/abi/ring.h"
#include "midix/abi/status.h"
#include "midix/abi/types.h"

constexpr mx_u32_t MX_PROTOCOL_GPU = 0x4750'5500u; // 'GPU\0'
constexpr mx_u16_t MX_GPU_VERSION_MAJOR = 1;
constexpr mx_u16_t MX_GPU_VERSION_MINOR = 0;

// The bootstrap opcodes are contiguous from the protocol base; the future
// opcodes named in spec §13.6 occupy a reserved block behind them. Both ranges
// stay below MX_GPU_OPCODE_FUTURE_LIMIT so later additions never renumber a
// shipped bootstrap opcode.
constexpr mx_u32_t MX_GPU_OPCODE_FUTURE_BASE = 0x4750'5580u;
constexpr mx_u32_t MX_GPU_OPCODE_FUTURE_LIMIT = 0x4750'55FFu;

enum MxGpuOpcode : mx_u32_t {
    // --- Required bootstrap opcodes (spec §13.5) ---------------------------
    GPU_HELLO = 0x4750'5501u,
    GPU_ENUM_OUTPUTS = 0x4750'5502u,
    GPU_GET_MODES = 0x4750'5503u,
    GPU_BUFFER_REGISTER = 0x4750'5504u,
    GPU_BUFFER_UNREGISTER = 0x4750'5505u,
    GPU_SET_MODE = 0x4750'5506u,
    GPU_PRESENT = 0x4750'5507u,
    GPU_PRESENT_COMPLETE = 0x4750'5508u,
    GPU_ERROR = 0x4750'5509u,

    // --- Reserved range for the future opcodes (spec §13.6) ----------------
    GPU_BUFFER_CREATE = 0x4750'5580u,
    GPU_BUFFER_MAP = 0x4750'5581u,
    GPU_BUFFER_IMPORT = 0x4750'5582u,
    GPU_BUFFER_EXPORT = 0x4750'5583u,
    GPU_CONTEXT_CREATE = 0x4750'5584u,
    GPU_CONTEXT_DESTROY = 0x4750'5585u,
    GPU_SUBMIT = 0x4750'5586u,
    GPU_FENCE_WAIT = 0x4750'5587u,
    GPU_FENCE_SIGNALLED = 0x4750'5588u,
    GPU_CURSOR_SET = 0x4750'5589u,
    GPU_ATOMIC_COMMIT = 0x4750'558Au,
    GPU_GET_PROPERTIES = 0x4750'558Bu,
};

// Opaque, boot-generation-scoped remote object identifier.
using mx_gpu_object_id_t = mx_u64_t;

// Core object types (spec §13.4). Values tag a `mx_gpu_object_id_t` when a
// message needs to state what kind of object it refers to.
enum MxGpuObjectType : mx_u32_t {
    MX_GPU_OBJECT_DEVICE = 1,
    MX_GPU_OBJECT_OUTPUT = 2,
    MX_GPU_OBJECT_MODE = 3,
    MX_GPU_OBJECT_BUFFER = 4,
    MX_GPU_OBJECT_FRAMEBUFFER = 5,
    MX_GPU_OBJECT_CONTEXT = 6,
    MX_GPU_OBJECT_FENCE = 7,
};

// Feature bits advertised by GPU_HELLO and by the device. Optional opcodes are
// only used when the corresponding bit is set (spec §13.6).
enum MxGpuFeatureBits : mx_u64_t {
    MX_GPU_FEATURE_STAGING_BUFFER = 1ull << 0, // GPU_BUFFER_REGISTER staging path
    MX_GPU_FEATURE_ASYNC_PRESENT = 1ull << 1,  // GPU_PRESENT is asynchronous
    MX_GPU_FEATURE_DAMAGE_RECTS = 1ull << 2,   // damage rectangles are honoured
    MX_GPU_FEATURE_CURSOR = 1ull << 3,         // future GPU_CURSOR_SET
    MX_GPU_FEATURE_ATOMIC_COMMIT = 1ull << 4,  // future GPU_ATOMIC_COMMIT
    MX_GPU_FEATURE_BUFFER_EXPORT = 1ull << 5,  // future GPU_BUFFER_EXPORT
    MX_GPU_FEATURE_RENDER_CONTEXT = 1ull << 6, // future GPU_CONTEXT_* / GPU_SUBMIT
    MX_GPU_FEATURE_REMOTE_VULKAN = 1ull << 7,  // remote Vulkan session transport
};

enum MxGpuHelloFlags : mx_u32_t {
    MX_GPU_HELLO_FLAG_NONE = 0,
};

enum MxGpuPresentFlags : mx_u32_t {
    MX_GPU_PRESENT_FLAG_NONE = 0,
    MX_GPU_PRESENT_FLAG_FULL_FRAME = 1u << 0, // damage is the whole buffer
    MX_GPU_PRESENT_FLAG_ASYNC = 1u << 1,      // completion via GPU_PRESENT_COMPLETE
};

enum MxGpuBufferFlags : mx_u32_t {
    MX_GPU_BUFFER_FLAG_NONE = 0,
    MX_GPU_BUFFER_FLAG_STAGING = 1u << 0, // Dango shared staging buffer
    MX_GPU_BUFFER_FLAG_SCANOUT = 1u << 1,
};

// Pixel formats are Midix-native, never DRM fourccs. XRGB8888 matches the
// bootstrap shared staging scanout buffer (spec §12.2).
enum MxGpuPixelFormat : mx_u32_t {
    MX_GPU_PIXFMT_XRGB8888 = 1,
};

// A single inline damage rectangle. The bootstrap protocol carries at most
// MX_GPU_MAX_INLINE_DAMAGE of them; larger lists belong in a shared arena.
struct MxGpuDamageRect {
    mx_u32_t x;
    mx_u32_t y;
    mx_u32_t width;
    mx_u32_t height;
};
static_assert(sizeof(MxGpuDamageRect) == 16, "damage rectangle must be 16 bytes");

constexpr mx_u32_t MX_GPU_MAX_INLINE_DAMAGE = 1;

// --- GPU_HELLO: version negotiation (spec §13.3) ---------------------------

struct MxGpuHello {
    mx_u16_t major;
    mx_u16_t minor;
    mx_u32_t flags;
    mx_u64_t feature_bits;
    mx_u64_t boot_generation;
};
static_assert(sizeof(MxGpuHello) <= MX_RING_INLINE_PAYLOAD, "GPU_HELLO must fit one payload");
static_assert(sizeof(MxGpuHello) == 24, "GPU_HELLO size drift");

struct MxGpuHelloReply {
    mx_status_t status;
    mx_u16_t major;
    mx_u16_t minor;
    mx_u32_t flags;
    mx_u64_t feature_bits;
    mx_u64_t boot_generation;
};
static_assert(sizeof(MxGpuHelloReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_HELLO reply must fit one payload");
static_assert(sizeof(MxGpuHelloReply) == 32, "GPU_HELLO reply size drift");

// --- GPU_ENUM_OUTPUTS -------------------------------------------------------

struct MxGpuEnumOutputsRequest {
    mx_gpu_object_id_t device_id;
    mx_u64_t boot_generation;
    mx_u32_t index;
    mx_u32_t flags;
};
static_assert(sizeof(MxGpuEnumOutputsRequest) <= MX_RING_INLINE_PAYLOAD,
              "GPU_ENUM_OUTPUTS request must fit one payload");
static_assert(sizeof(MxGpuEnumOutputsRequest) == 24, "GPU_ENUM_OUTPUTS request size drift");

struct MxGpuEnumOutputsReply {
    mx_status_t status;
    mx_gpu_object_id_t output_id;
    mx_u32_t output_count;
    mx_u32_t output_type;
    mx_u32_t connection;
    mx_u32_t width_mm;
    mx_u32_t height_mm;
    mx_u32_t flags;
    mx_u8_t name[8];
};
static_assert(sizeof(MxGpuEnumOutputsReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_ENUM_OUTPUTS reply must fit one payload");
static_assert(sizeof(MxGpuEnumOutputsReply) == 48, "GPU_ENUM_OUTPUTS reply size drift");

// --- GPU_GET_MODES ----------------------------------------------------------

struct MxGpuGetModesRequest {
    mx_gpu_object_id_t output_id;
    mx_u64_t boot_generation;
    mx_u32_t index;
    mx_u32_t flags;
};
static_assert(sizeof(MxGpuGetModesRequest) <= MX_RING_INLINE_PAYLOAD,
              "GPU_GET_MODES request must fit one payload");
static_assert(sizeof(MxGpuGetModesRequest) == 24, "GPU_GET_MODES request size drift");

struct MxGpuGetModesReply {
    mx_status_t status;
    mx_gpu_object_id_t mode_id;
    mx_u32_t mode_count;
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t refresh_mhz;
    mx_u32_t pixel_format;
    mx_u32_t flags;
    mx_u8_t name[8];
};
static_assert(sizeof(MxGpuGetModesReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_GET_MODES reply must fit one payload");
static_assert(sizeof(MxGpuGetModesReply) == 48, "GPU_GET_MODES reply size drift");

// --- GPU_BUFFER_REGISTER ----------------------------------------------------

struct MxGpuBufferRegisterRequest {
    mx_u64_t shm_id; // Dango shared staging/arena identifier
    mx_u64_t boot_generation;
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t stride;
    mx_u32_t pixel_format;
    mx_u32_t flags;
    mx_u32_t reserved0;
};
static_assert(sizeof(MxGpuBufferRegisterRequest) <= MX_RING_INLINE_PAYLOAD,
              "GPU_BUFFER_REGISTER request must fit one payload");
static_assert(sizeof(MxGpuBufferRegisterRequest) == 40, "GPU_BUFFER_REGISTER request size drift");

struct MxGpuBufferRegisterReply {
    mx_status_t status;
    mx_gpu_object_id_t buffer_id;
    mx_u64_t boot_generation;
    mx_u32_t reserved0;
    mx_u32_t reserved1;
};
static_assert(sizeof(MxGpuBufferRegisterReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_BUFFER_REGISTER reply must fit one payload");
static_assert(sizeof(MxGpuBufferRegisterReply) == 32, "GPU_BUFFER_REGISTER reply size drift");

// --- GPU_BUFFER_UNREGISTER --------------------------------------------------

struct MxGpuBufferUnregisterRequest {
    mx_gpu_object_id_t buffer_id;
    mx_u64_t boot_generation;
};
static_assert(sizeof(MxGpuBufferUnregisterRequest) <= MX_RING_INLINE_PAYLOAD,
              "GPU_BUFFER_UNREGISTER request must fit one payload");
static_assert(sizeof(MxGpuBufferUnregisterRequest) == 16,
              "GPU_BUFFER_UNREGISTER request size drift");

struct MxGpuBufferUnregisterReply {
    mx_status_t status;
    mx_u64_t boot_generation;
};
static_assert(sizeof(MxGpuBufferUnregisterReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_BUFFER_UNREGISTER reply must fit one payload");
static_assert(sizeof(MxGpuBufferUnregisterReply) == 16, "GPU_BUFFER_UNREGISTER reply size drift");

// --- GPU_SET_MODE -----------------------------------------------------------

struct MxGpuSetModeRequest {
    mx_gpu_object_id_t output_id;
    mx_gpu_object_id_t mode_id;
    mx_gpu_object_id_t buffer_id;
    mx_u64_t boot_generation;
    mx_u32_t flags;
    mx_u32_t reserved0;
};
static_assert(sizeof(MxGpuSetModeRequest) <= MX_RING_INLINE_PAYLOAD,
              "GPU_SET_MODE request must fit one payload");
static_assert(sizeof(MxGpuSetModeRequest) == 40, "GPU_SET_MODE request size drift");

struct MxGpuSetModeReply {
    mx_status_t status;
    mx_gpu_object_id_t framebuffer_id;
    mx_u64_t boot_generation;
};
static_assert(sizeof(MxGpuSetModeReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_SET_MODE reply must fit one payload");
static_assert(sizeof(MxGpuSetModeReply) == 24, "GPU_SET_MODE reply size drift");

// --- GPU_PRESENT (asynchronous; spec §13.5) --------------------------------

struct MxGpuPresentRequest {
    mx_gpu_object_id_t buffer_id;
    mx_gpu_object_id_t framebuffer_id;
    mx_u64_t boot_generation;
    mx_u32_t flags;
    mx_u32_t damage_count; // 0 => whole framebuffer, else `damage` is valid
    MxGpuDamageRect damage;
};
static_assert(sizeof(MxGpuPresentRequest) <= MX_RING_INLINE_PAYLOAD,
              "GPU_PRESENT request must fit one payload");
static_assert(sizeof(MxGpuPresentRequest) == 48, "GPU_PRESENT request size drift");

// Enqueue acknowledgement: success only means the appliance accepted the
// operation, not that it completed.
struct MxGpuPresentReply {
    mx_status_t status;
    mx_gpu_object_id_t buffer_id;
    mx_u64_t boot_generation;
    mx_u32_t reserved0;
    mx_u32_t reserved1;
};
static_assert(sizeof(MxGpuPresentReply) <= MX_RING_INLINE_PAYLOAD,
              "GPU_PRESENT reply must fit one payload");
static_assert(sizeof(MxGpuPresentReply) == 32, "GPU_PRESENT reply size drift");

// Completion notification. The slot `seq` carries the original request token so
// the client can correlate the completion with its outstanding enqueue.
struct MxGpuPresentComplete {
    mx_status_t status;
    mx_gpu_object_id_t buffer_id;
    mx_u64_t boot_generation;
    mx_u32_t flags;
    mx_u32_t reserved0;
};
static_assert(sizeof(MxGpuPresentComplete) <= MX_RING_INLINE_PAYLOAD,
              "GPU_PRESENT_COMPLETE must fit one payload");
static_assert(sizeof(MxGpuPresentComplete) == 32, "GPU_PRESENT_COMPLETE size drift");

// --- GPU_ERROR --------------------------------------------------------------

struct MxGpuError {
    mx_status_t status;
    mx_gpu_object_id_t object_id; // object the failure refers to, or 0
    mx_u64_t boot_generation;
    mx_u64_t detail;
    mx_u32_t opcode; // opcode that failed
    mx_u32_t flags;
    mx_u32_t reserved0;
    mx_u32_t reserved1;
};
static_assert(sizeof(MxGpuError) <= MX_RING_INLINE_PAYLOAD, "GPU_ERROR must fit one payload");
static_assert(sizeof(MxGpuError) == 48, "GPU_ERROR size drift");

// The control-ring frame for this protocol is the shared ring slot verbatim.
using MxGpuFrame = MxRingSlot;
static_assert(sizeof(MxGpuFrame) == 64, "GPU frame must be one ring slot");
static_assert(MX_RING_INLINE_PAYLOAD == 48, "GPU payload budget changed");

// --- Compile-time invariants ------------------------------------------------

// Every opcode value is unique. Enumerated in one place so drift (a copy/paste
// duplicate or a renumbered opcode) fails the build in every translation unit.
constexpr mx_u32_t kMxGpuOpcodeValues[] = {
    GPU_HELLO,           GPU_ENUM_OUTPUTS,      GPU_GET_MODES,
    GPU_BUFFER_REGISTER, GPU_BUFFER_UNREGISTER, GPU_SET_MODE,
    GPU_PRESENT,         GPU_PRESENT_COMPLETE,  GPU_ERROR,
    GPU_BUFFER_CREATE,   GPU_BUFFER_MAP,        GPU_BUFFER_IMPORT,
    GPU_BUFFER_EXPORT,   GPU_CONTEXT_CREATE,    GPU_CONTEXT_DESTROY,
    GPU_SUBMIT,          GPU_FENCE_WAIT,        GPU_FENCE_SIGNALLED,
    GPU_CURSOR_SET,      GPU_ATOMIC_COMMIT,     GPU_GET_PROPERTIES,
};

constexpr bool mx_gpu_opcodes_unique() {
    constexpr mx_size_t count = sizeof(kMxGpuOpcodeValues) / sizeof(kMxGpuOpcodeValues[0]);
    for (mx_size_t i = 0; i < count; ++i) {
        for (mx_size_t j = i + 1; j < count; ++j) {
            if (kMxGpuOpcodeValues[i] == kMxGpuOpcodeValues[j]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(mx_gpu_opcodes_unique(), "GPU opcode values must be unique");

// The bootstrap opcodes stay ahead of the reserved future block, and the future
// block stays inside its reserved range.
static_assert(GPU_ERROR < MX_GPU_OPCODE_FUTURE_BASE,
              "bootstrap opcodes must precede the reserved future range");
static_assert(GPU_BUFFER_CREATE >= MX_GPU_OPCODE_FUTURE_BASE &&
                  GPU_GET_PROPERTIES <= MX_GPU_OPCODE_FUTURE_LIMIT,
              "future opcodes must live in the reserved range");

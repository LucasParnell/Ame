#pragma once
// Midix remote Vulkan session transport ABI (Ame spec §14.6/§14.7, Phase N/O).
//
// This is the contract between a native Mochi Vulkan client (the `ame_remote`
// ICD) and the native GPU service (`drv.gpu.linux`, spec §15.1/§15.2) that
// brokers one *session* per client towards the guest `dango-vkd` appliance.
//
// Properties, and why they hold:
//
//   * No Linux pointers, fds, DRM ioctls or virtio-gpu registers ever appear on
//     the wire (spec §14.6). Every object is an opaque, boot-generation-scoped
//     64-bit id; only the guest `dango-vkd` may map one to a Vulkan/DRM handle.
//   * Fixed capacity, no hot-path allocation (spec §14.7). A session carries a
//     bounded command stream and a bounded object table; the sizes below are the
//     only place they are stated.
//   * Not one RPC per call. A recorded command buffer is appended to the
//     session's shared command region and crossed to the appliance with one
//     VK_SESSION_SUBMIT; sync is observed with one VK_SESSION_SYNC_WAIT.
//   * Bounded and endian-defined. Every inline message fits the 48-byte ring
//     payload; every multi-byte field is little-endian exactly as the rest of
//     the Dango ring contract is (see the endianness note below).
//
// Transport envelope: MxRingSlot, identical to every other Mochi service ring.
// `type` is the opcode, `length` bounds the inline payload, `seq` is the
// producer's running ring index (the correlation token), exactly as the GPU and
// GPU-service protocols use it. Larger structures live in the session arena, a
// shared window the session opens on and the client writes directly.

// The shared value types (MxGpuDamageRect, the 48-byte inline budget), the
// C/C++ `MX_STATIC_ASSERT` macro and the ring-slot alias live in the appliance
// header, which is C/C++ compatible for exactly this reason.
#include "midix/abi/protocols/gpu.h"

// C has no struct-tag elision: give every wire struct its typedef up front so
// plain `MxVkX` reads the same in C and C++ (the same discipline protocols/gpu.h
// uses for its wire types).
typedef struct MxVkSessionOpenRequest MxVkSessionOpenRequest;
typedef struct MxVkSessionOpenReply MxVkSessionOpenReply;
typedef struct MxVkSessionCloseRequest MxVkSessionCloseRequest;
typedef struct MxVkSessionCloseReply MxVkSessionCloseReply;
typedef struct MxVkSessionAllocRequest MxVkSessionAllocRequest;
typedef struct MxVkSessionAllocReply MxVkSessionAllocReply;
typedef struct MxVkSessionFreeRequest MxVkSessionFreeRequest;
typedef struct MxVkSessionFreeReply MxVkSessionFreeReply;
typedef struct MxVkSessionSubmitRequest MxVkSessionSubmitRequest;
typedef struct MxVkSessionSubmitReply MxVkSessionSubmitReply;
typedef struct MxVkSessionSyncWaitRequest MxVkSessionSyncWaitRequest;
typedef struct MxVkSessionSyncWaitReply MxVkSessionSyncWaitReply;
typedef struct MxVkSurfaceCreateRequest MxVkSurfaceCreateRequest;
typedef struct MxVkSurfaceCreateReply MxVkSurfaceCreateReply;
typedef struct MxVkSurfaceDestroyRequest MxVkSurfaceDestroyRequest;
typedef struct MxVkSurfaceDestroyReply MxVkSurfaceDestroyReply;
typedef struct MxVkSwapchainCreateRequest MxVkSwapchainCreateRequest;
typedef struct MxVkSwapchainCreateReply MxVkSwapchainCreateReply;
typedef struct MxVkSwapchainAcquireRequest MxVkSwapchainAcquireRequest;
typedef struct MxVkSwapchainAcquireReply MxVkSwapchainAcquireReply;
typedef struct MxVkSwapchainPresentRequest MxVkSwapchainPresentRequest;
typedef struct MxVkSwapchainPresentReply MxVkSwapchainPresentReply;
typedef struct MxVkSessionError MxVkSessionError;
typedef struct MxVkCommandHeader MxVkCommandHeader;
typedef struct MxVkCmdFillBuffer MxVkCmdFillBuffer;
typedef struct MxVkCmdCopyBuffer MxVkCmdCopyBuffer;
typedef struct MxVkCmdDispatch MxVkCmdDispatch;
typedef struct MxVkCmdBindVertexBuffer MxVkCmdBindVertexBuffer;
typedef struct MxVkCmdBindRenderTarget MxVkCmdBindRenderTarget;
typedef struct MxVkCmdDraw MxVkCmdDraw;

// The protocol base id (spec §14.6). Distinct from both the appliance control
// plane and the native GPU service face.
#define MX_PROTOCOL_VULKAN_SESSION 0x564B5300u // 'VKS\0'
#define MX_VK_SESSION_VERSION_MAJOR 1
#define MX_VK_SESSION_VERSION_MINOR 0

// The session protocol reuses the appliance envelope verbatim; state it once.
#define MX_VK_SESSION_INLINE_PAYLOAD MX_GPU_INLINE_PAYLOAD

// The wire is little-endian, matching AArch64 Mochi, the Linux guest and the
// Dango ring contract. Multi-byte fields are exchanged in this order; a build
// on a big-endian host would have to byte-swap at the marshalling boundary, so
// refuse to compile there rather than silently corrupt the wire.
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "the Midix Vulkan session wire format is little-endian"
#endif

enum MxVulkanSessionOpcode {
    // --- Session lifecycle -------------------------------------------------
    VK_SESSION_OPEN = 0x564B5301u,           // Open -> OPEN_REPLY
    VK_SESSION_OPEN_REPLY = 0x564B5302u,
    VK_SESSION_CLOSE = 0x564B5303u,          // Close -> CLOSE_REPLY
    VK_SESSION_CLOSE_REPLY = 0x564B5304u,

    // --- Host-visible memory ----------------------------------------------
    VK_SESSION_ALLOC = 0x564B5305u,          // Alloc -> ALLOC_REPLY
    VK_SESSION_ALLOC_REPLY = 0x564B5306u,
    VK_SESSION_FREE = 0x564B5307u,           // Free -> FREE_REPLY
    VK_SESSION_FREE_REPLY = 0x564B5308u,

    // --- Bounded command submission + sync --------------------------------
    VK_SESSION_SUBMIT = 0x564B5309u,         // Submit -> SUBMIT_REPLY
    VK_SESSION_SUBMIT_REPLY = 0x564B530Au,
    VK_SESSION_SYNC_WAIT = 0x564B530Bu,      // SyncWait -> SYNC_WAIT_REPLY
    VK_SESSION_SYNC_WAIT_REPLY = 0x564B530Cu,

    // --- WSI: surface / swapchain (spec §15.3–§15.6) ----------------------
    VK_SESSION_SURFACE_CREATE = 0x564B530Du, // SurfaceCreate -> ..._REPLY
    VK_SESSION_SURFACE_CREATE_REPLY = 0x564B530Eu,
    VK_SESSION_SURFACE_DESTROY = 0x564B530Fu,
    VK_SESSION_SURFACE_DESTROY_REPLY = 0x564B5310u,
    VK_SESSION_SWAPCHAIN_CREATE = 0x564B5311u,
    VK_SESSION_SWAPCHAIN_CREATE_REPLY = 0x564B5312u,
    VK_SESSION_SWAPCHAIN_ACQUIRE = 0x564B5313u,
    VK_SESSION_SWAPCHAIN_ACQUIRE_REPLY = 0x564B5314u,
    // Present carries the bounded copy of one swapchain image into the Dango
    // shared buffer; the reply reports how many bytes crossed (spec §15.4).
    VK_SESSION_SWAPCHAIN_PRESENT = 0x564B5315u,
    VK_SESSION_SWAPCHAIN_PRESENT_REPLY = 0x564B5316u,

    // --- Failure -----------------------------------------------------------
    // Rejected request; `opcode` names the request the service refused.
    VK_SESSION_ERROR = 0x564B53FFu,
};

// --- Fixed capacities and quotas (spec §14.7) -------------------------------
//
// One place for every bound. The broker enforces each of them; the client sizes
// its own fixed tables from the same constants, so the two cannot drift.

// Ring geometry is fixed by the session ABI: a session ring is a normal Mochi
// ring, so its order is the Midix ring default (no negotiation, no drift). The
// C view reads the byte-exact Dango counterpart, which the channel header already
// pins equal to the Midix default.
#if defined(__cplusplus)
#define MX_VK_SESSION_RING_ORDER MX_DEFAULT_RING_ORDER
#else
#define MX_VK_SESSION_RING_ORDER DANGO_CH_DEFAULT_RING_ORDER
#endif
#define MX_VK_SESSION_SLOTS (1u << MX_VK_SESSION_RING_ORDER)

// Sessions one GPU service brokers at once, and the per-session quotas the
// broker will never exceed regardless of what a client asks for.
#define MX_VK_SESSION_MAX_SESSIONS 8u
#define MX_VK_SESSION_MAX_OBJECTS 64u
#define MX_VK_SESSION_MAX_OUTSTANDING 16u
#define MX_VK_SESSION_MAX_SURFACES 4u
#define MX_VK_SESSION_MAX_SWAPCHAINS 4u
#define MX_VK_SWAPCHAIN_MAX_IMAGES 3u

// The per-session arena: one shared window holding the command region and every
// host-visible allocation. Both ends bound it from here.
#define MX_VK_SESSION_DEFAULT_ARENA_BYTES (1u << 20)  // 1 MiB
#define MX_VK_SESSION_MAX_ARENA_BYTES (16u << 20)     // 16 MiB
#define MX_VK_SESSION_ARENA_ALIGNMENT 256u

// The command region is the head of the arena: a bounded byte stream of
// MxVkCommandHeader-prefixed records (see the command vocabulary below). A
// submit names a [offset, offset+bytes) range inside it.
#define MX_VK_COMMAND_REGION_BYTES (64u << 10) // 64 KiB
#define MX_VK_MAX_COMMAND_BYTES 64u            // no single record exceeds this
#define MX_VK_COMMAND_ALIGNMENT 8u
#define MX_VK_MAX_COMMANDS_PER_SUBMIT 1024u

// The Dango shared buffer a WSI present copies through (spec §15.4). The
// bootstrap frame the appliance scans out is exactly this large, so a swapchain
// image that fits the staging window can be presented in one bounded copy.
#define MX_VK_SHARED_BUFFER_MAX_BYTES (4u << 20) // 4 MiB

enum MxVulkanSessionFlags {
    MX_VK_SESSION_FLAG_NONE = 0,
    // Advertised by OPEN_REPLY: the session's arena is mapped and the client may
    // write its command region directly. Absent => the client stays local.
    MX_VK_SESSION_FLAG_ARENA_READY = 1u << 0,
    // Advertised by OPEN_REPLY: the appliance supports the WSI path (spec §15.3).
    MX_VK_SESSION_FLAG_WSI = 1u << 1,
    // Advertised by OPEN_REPLY: the appliance drives a real Vulkan device
    // (lavapipe today). Absent => submits are refused with
    // MX_ERR_SERVICE_UNAVAILABLE and the client degrades cleanly.
    MX_VK_SESSION_FLAG_VULKAN_LIVE = 1u << 2,
};

enum MxVkMemoryType {
    MX_VK_MEMORY_TYPE_HOST_VISIBLE = 1, // client-writable, arena-backed
    MX_VK_MEMORY_TYPE_DEVICE_LOCAL = 2, // appliance-private
};

enum MxVkMemoryFlags {
    MX_VK_MEMORY_FLAG_NONE = 0,
    MX_VK_MEMORY_FLAG_MAPPED = 1u << 0, // client has a writable view of the region
};

enum MxVkSubmitFlags {
    MX_VK_SUBMIT_FLAG_NONE = 0,
    MX_VK_SUBMIT_FLAG_SIGNAL = 1u << 0, // signal sync_id on completion
};

enum MxVkSwapchainFlags {
    MX_VK_SWAPCHAIN_FLAG_NONE = 0,
    MX_VK_SWAPCHAIN_FLAG_BOUNDED_COPY = 1u << 0, // present copies, never zero-copy
};

enum MxVkPresentFlags {
    MX_VK_PRESENT_FLAG_NONE = 0,
    MX_VK_PRESENT_FLAG_FULL_FRAME = 1u << 0, // copy/scan out the whole image
};

// --- Opaque object ids ------------------------------------------------------
//
// [ generation:32 | reserved:12 | session:4 | type:4 | index:12 ]
//
// A session id and every object id it mints embed the boot generation and the
// session index, so an id from another boot, another session or another object
// kind fails validation instead of aliasing a live object (spec §14.7). Both
// ends share the bit layout and the two helpers below so no side re-derives it.
enum MxVkObjectType {
    MX_VK_OBJECT_SESSION = 1,
    MX_VK_OBJECT_MEMORY = 2,
    MX_VK_OBJECT_SURFACE = 3,
    MX_VK_OBJECT_SWAPCHAIN = 4,
};

#define MX_VK_OBJECT_INDEX_BITS 12
#define MX_VK_OBJECT_TYPE_BITS 4
#define MX_VK_OBJECT_SESSION_BITS 4
#define MX_VK_OBJECT_INDEX_MASK ((1ull << MX_VK_OBJECT_INDEX_BITS) - 1ull)
#define MX_VK_OBJECT_TYPE_SHIFT MX_VK_OBJECT_INDEX_BITS
#define MX_VK_OBJECT_TYPE_MASK (((1ull << MX_VK_OBJECT_TYPE_BITS) - 1ull) << MX_VK_OBJECT_TYPE_SHIFT)
#define MX_VK_OBJECT_SESSION_SHIFT (MX_VK_OBJECT_INDEX_BITS + MX_VK_OBJECT_TYPE_BITS)
#define MX_VK_OBJECT_SESSION_MASK \
    (((1ull << MX_VK_OBJECT_SESSION_BITS) - 1ull) << MX_VK_OBJECT_SESSION_SHIFT)
// The 12 bits between the session and boot-generation fields distinguish
// reused table slots. A saturated slot must be retired, never wrapped.
#define MX_VK_OBJECT_INCARNATION_BITS 12
#define MX_VK_OBJECT_INCARNATION_SHIFT (MX_VK_OBJECT_INDEX_BITS + MX_VK_OBJECT_TYPE_BITS + MX_VK_OBJECT_SESSION_BITS)
#define MX_VK_OBJECT_MAX_INCARNATION ((1u << MX_VK_OBJECT_INCARNATION_BITS) - 1u)
#define MX_VK_OBJECT_GENERATION_SHIFT (MX_VK_OBJECT_INCARNATION_SHIFT + MX_VK_OBJECT_INCARNATION_BITS)

// 1-based index; 0 is never a valid object slot.
static inline mx_u64_t mx_vk_object_id_make(mx_u64_t generation, mx_u32_t type,
                                            mx_u32_t session, mx_u32_t index) {
    return (((generation & 0xFFFFFFFFull) << MX_VK_OBJECT_GENERATION_SHIFT)) |
           (((mx_u64_t)session << MX_VK_OBJECT_SESSION_SHIFT) & MX_VK_OBJECT_SESSION_MASK) |
           (((mx_u64_t)type << MX_VK_OBJECT_TYPE_SHIFT) & MX_VK_OBJECT_TYPE_MASK) |
           ((mx_u64_t)index & MX_VK_OBJECT_INDEX_MASK);
}

// The broker compares the full id after decoding its table index.
static inline mx_u64_t mx_vk_object_id_with_incarnation(mx_u64_t id, mx_u32_t incarnation) {
    return id | ((mx_u64_t)(incarnation & MX_VK_OBJECT_MAX_INCARNATION)
                 << MX_VK_OBJECT_INCARNATION_SHIFT);
}

// Decode `id`, confirm it names `type` in generation `generation` for `session`,
// and store the 1-based index. Returns 0 on success, -1 on a stale, mistyped,
// cross-session or null id.
static inline int mx_vk_object_id_check(mx_u64_t id, mx_u32_t type, mx_u64_t generation,
                                        mx_u32_t session, mx_u32_t *index_out) {
    const mx_u32_t index = (mx_u32_t)(id & MX_VK_OBJECT_INDEX_MASK);
    const mx_u32_t id_type = (mx_u32_t)((id & MX_VK_OBJECT_TYPE_MASK) >> MX_VK_OBJECT_TYPE_SHIFT);
    const mx_u32_t id_session =
        (mx_u32_t)((id & MX_VK_OBJECT_SESSION_MASK) >> MX_VK_OBJECT_SESSION_SHIFT);
    const mx_u64_t id_generation = id >> MX_VK_OBJECT_GENERATION_SHIFT;
    if (index == 0 || id_type != type || id_session != session ||
        id_generation != (generation & 0xFFFFFFFFull)) {
        return -1;
    }
    if (index_out) *index_out = index;
    return 0;
}

// The largest index the id layout can express for any object.
#define MX_VK_OBJECT_MAX_INDEX MX_VK_OBJECT_INDEX_MASK
MX_STATIC_ASSERT(MX_VK_SESSION_MAX_OBJECTS <= MX_VK_OBJECT_MAX_INDEX,
                 "object quota must fit the id index field");
MX_STATIC_ASSERT(MX_VK_SESSION_MAX_SESSIONS <= ((1u << MX_VK_OBJECT_SESSION_BITS) - 1u),
                 "session quota must fit the id session field");

// --- Session lifecycle messages ---------------------------------------------

// A native client opens exactly one session per shared ring pair. It asks for
// an arena size, an object count and an outstanding-request count; the service
// grants at most the quotas above (a client that asks for more is not refused,
// it is capped, and the granted values come back in the reply).
struct MxVkSessionOpenRequest {
    mx_u16_t major;
    mx_u16_t minor;
    mx_u32_t flags;         // MxVulkanSessionFlags (request: must be 0 today)
    mx_u64_t arena_bytes;   // requested arena, capped at MX_VK_SESSION_MAX_ARENA_BYTES
    mx_u32_t max_objects;   // requested object table, capped at MAX_OBJECTS
    mx_u32_t max_outstanding; // requested in-flight submits, capped at MAX_OUTSTANDING
    mx_u32_t client_id;     // native client identity the broker pins the session to
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionOpenRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_OPEN must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionOpenRequest) == 32, "VK_SESSION_OPEN size drift");

struct MxVkSessionOpenReply {
    mx_status_t status;
    mx_u64_t session_id;       // opaque; echoes the client_id in its id layout
    mx_u64_t boot_generation;
    mx_u64_t arena_id;         // Dango shared window id backing the arena
    mx_u32_t arena_bytes;      // granted (>= command region, <= max)
    mx_u32_t max_objects;      // granted
    mx_u32_t max_outstanding;  // granted
    mx_u32_t flags;            // MxVulkanSessionFlags
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionOpenReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_OPEN reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionOpenReply) == 48, "VK_SESSION_OPEN reply size drift");

struct MxVkSessionCloseRequest {
    mx_u64_t session_id;
    mx_u32_t flags;
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionCloseRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_CLOSE must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionCloseRequest) == 16, "VK_SESSION_CLOSE size drift");

struct MxVkSessionCloseReply {
    mx_status_t status;
    mx_u64_t session_id;
    mx_u64_t boot_generation;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionCloseReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_CLOSE reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionCloseReply) == 24, "VK_SESSION_CLOSE reply size drift");

// --- Host-visible memory ----------------------------------------------------

struct MxVkSessionAllocRequest {
    mx_u64_t session_id;
    mx_u64_t size;
    mx_u32_t alignment;    // 0 => the arena default
    mx_u32_t memory_type;  // MxVkMemoryType
    mx_u32_t usage;        // client-defined usage mask (opaque to the appliance)
    mx_u32_t flags;        // MxVkMemoryFlags
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionAllocRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_ALLOC must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionAllocRequest) == 32, "VK_SESSION_ALLOC size drift");

struct MxVkSessionAllocReply {
    mx_status_t status;
    mx_u64_t memory_id;      // opaque, boot/session-scoped
    mx_u64_t boot_generation;
    mx_u64_t arena_offset;   // byte offset into the session arena
    mx_u32_t size;           // granted size (rounded up to alignment)
    mx_u32_t memory_type;
    mx_u32_t alignment;
    mx_u32_t flags;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionAllocReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_ALLOC reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionAllocReply) == 48, "VK_SESSION_ALLOC reply size drift");

struct MxVkSessionFreeRequest {
    mx_u64_t session_id;
    mx_u64_t memory_id;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionFreeRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_FREE must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionFreeRequest) == 16, "VK_SESSION_FREE size drift");

struct MxVkSessionFreeReply {
    mx_status_t status;
    mx_u64_t memory_id;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionFreeReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_FREE reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionFreeReply) == 16, "VK_SESSION_FREE reply size drift");

// --- Bounded command submission + sync --------------------------------------

// Submit names a byte range of the session command region (the arena head). The
// commands themselves were recorded by the client directly into the shared
// arena, so one submit crosses an arbitrary number of commands: this is the
// "shared rings, not one RPC per call" property (spec §14.7).
struct MxVkSessionSubmitRequest {
    mx_u64_t session_id;
    mx_u64_t command_offset; // byte offset into the command region
    mx_u64_t sync_id;        // client token signalled on completion (0 => none)
    mx_u32_t command_bytes;  // <= MX_VK_COMMAND_REGION_BYTES, record-aligned
    mx_u32_t flags;          // MxVkSubmitFlags
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSubmitRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SUBMIT must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSubmitRequest) == 32, "VK_SESSION_SUBMIT size drift");

struct MxVkSessionSubmitReply {
    mx_status_t status;
    mx_u64_t session_id;
    mx_u64_t boot_generation;
    mx_u64_t sync_id;
    mx_u32_t command_bytes; // bytes the appliance accepted
    mx_u32_t outstanding;   // in-flight submits after this one
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSubmitReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SUBMIT reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSubmitReply) == 40, "VK_SESSION_SUBMIT reply size drift");

struct MxVkSessionSyncWaitRequest {
    mx_u64_t session_id;
    mx_u64_t sync_id;
    mx_u64_t timeout_ns; // 0 => poll once; the service bounds the real wait
    mx_u32_t flags;      // reserved; must be 0 today
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSyncWaitRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SYNC_WAIT must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSyncWaitRequest) == 32, "VK_SESSION_SYNC_WAIT size drift");

struct MxVkSessionSyncWaitReply {
    mx_status_t status; // MX_OK signalled; MX_ERR_BUSY not yet; MX_ERR_DEAD lost
    mx_u64_t session_id;
    mx_u64_t boot_generation;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSyncWaitReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SYNC_WAIT reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionSyncWaitReply) == 24,
                 "VK_SESSION_SYNC_WAIT reply size drift");

// --- WSI: surface / swapchain (spec §15.3–§15.6) ----------------------------

struct MxVkSurfaceCreateRequest {
    mx_u64_t session_id;
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t format;   // MxGpuPixelFormat (reused value type; single home)
    mx_u32_t flags;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceCreateRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SURFACE_CREATE must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceCreateRequest) == 24,
                 "VK_SESSION_SURFACE_CREATE size drift");

struct MxVkSurfaceCreateReply {
    mx_status_t status;
    mx_u64_t surface_id;
    mx_u64_t boot_generation;
    mx_u32_t width;
    mx_u32_t height;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceCreateReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SURFACE_CREATE reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceCreateReply) == 32,
                 "VK_SESSION_SURFACE_CREATE reply size drift");

struct MxVkSurfaceDestroyRequest {
    mx_u64_t session_id;
    mx_u64_t surface_id;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceDestroyRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SURFACE_DESTROY must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceDestroyRequest) == 16,
                 "VK_SESSION_SURFACE_DESTROY size drift");

struct MxVkSurfaceDestroyReply {
    mx_status_t status;
    mx_u64_t surface_id;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceDestroyReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SURFACE_DESTROY reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSurfaceDestroyReply) == 16,
                 "VK_SESSION_SURFACE_DESTROY reply size drift");

struct MxVkSwapchainCreateRequest {
    mx_u64_t session_id;
    mx_u64_t surface_id;
    mx_u32_t image_count; // capped at MX_VK_SWAPCHAIN_MAX_IMAGES
    mx_u32_t stride;      // bytes per row of a swapchain image
    mx_u32_t pixel_format;
    mx_u32_t flags;       // MxVkSwapchainFlags
};
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainCreateRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SWAPCHAIN_CREATE must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainCreateRequest) == 32,
                 "VK_SESSION_SWAPCHAIN_CREATE size drift");

struct MxVkSwapchainCreateReply {
    mx_status_t status;
    mx_u64_t swapchain_id;
    mx_u64_t boot_generation;
    mx_u32_t image_count; // granted
    mx_u32_t image_bytes; // per image
};
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainCreateReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SWAPCHAIN_CREATE reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainCreateReply) == 32,
                 "VK_SESSION_SWAPCHAIN_CREATE reply size drift");

struct MxVkSwapchainAcquireRequest {
    mx_u64_t session_id;
    mx_u64_t swapchain_id;
    mx_u64_t timeout_ns;
    mx_u32_t flags;
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainAcquireRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SWAPCHAIN_ACQUIRE must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainAcquireRequest) == 32,
                 "VK_SESSION_SWAPCHAIN_ACQUIRE size drift");

struct MxVkSwapchainAcquireReply {
    mx_status_t status;
    mx_u64_t swapchain_id;
    mx_u64_t image_id;      // opaque memory object backing the acquired image
    mx_u64_t arena_offset;  // image bytes inside the session arena
    mx_u32_t image_index;
    mx_u32_t image_bytes;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainAcquireReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SWAPCHAIN_ACQUIRE reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainAcquireReply) == 40,
                 "VK_SESSION_SWAPCHAIN_ACQUIRE size drift");

// Present copies the acquired image into the Dango shared buffer (bounded), and
// scanout stays with the native compositor / dango-gpud (spec §15.4). `damage`
// reuses the appliance value type so the two layers cannot drift.
struct MxVkSwapchainPresentRequest {
    mx_u64_t session_id;
    mx_u64_t swapchain_id;
    mx_u32_t image_index;
    mx_u32_t flags;        // MxVkPresentFlags
    MxGpuDamageRect damage; // valid for a bounded (non-full-frame) present
};
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainPresentRequest) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SWAPCHAIN_PRESENT must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainPresentRequest) == 40,
                 "VK_SESSION_SWAPCHAIN_PRESENT size drift");

struct MxVkSwapchainPresentReply {
    mx_status_t status;
    mx_u64_t swapchain_id;
    mx_u64_t frame_index;  // monotonic completed-present count
    mx_u32_t copied_bytes; // bytes the bounded copy moved into the shared buffer
    mx_u32_t image_index;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainPresentReply) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_SWAPCHAIN_PRESENT reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSwapchainPresentReply) == 32,
                 "VK_SESSION_SWAPCHAIN_PRESENT reply size drift");

// --- VK_SESSION_ERROR -------------------------------------------------------
struct MxVkSessionError {
    mx_status_t status;
    mx_u64_t session_id;
    mx_u64_t detail;  // request-specific (0 when none)
    mx_u32_t opcode;  // the request opcode the service refused
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkSessionError) <= MX_VK_SESSION_INLINE_PAYLOAD,
                 "VK_SESSION_ERROR must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxVkSessionError) == 32, "VK_SESSION_ERROR size drift");

// --- Command vocabulary (records in the session command region) -------------
//
// The command region is a byte stream of records: an MxVkCommandHeader, then
// the opcode-specific body, then padding to MX_VK_COMMAND_ALIGNMENT. Every
// record is <= MX_VK_MAX_COMMAND_BYTES, so a parser can always bound its walk.
// These are the transport-level mirror of a recorded Vulkan command buffer:
// the guest dango-vkd maps each one to the matching Vulkan call (or to the
// lavapipe software path). Keep the set minimal and explicit.
//
// Command operands name *session-arena byte offsets*, not opaque object ids:
// the arena is the one region both ends already share, so an offset is
// meaningful to the appliance without a second id-translation pass, and the
// per-session arena bounds every operand. The opaque memory object ids stay on
// the control plane (ALLOC/FREE/ACQUIRE/PRESENT), which is where lifetime and
// cross-session protection actually matter.

struct MxVkCommandHeader {
    mx_u32_t opcode; // MxVkCommandOpcode
    mx_u32_t size;   // whole record size, header included, 8-byte aligned
    mx_u64_t flags;  // command-specific
};
MX_STATIC_ASSERT(sizeof(struct MxVkCommandHeader) == 16, "command header size drift");

enum MxVkCommandOpcode {
    VK_CMD_NONE = 0,
    VK_CMD_PIPELINE_BARRIER = 1,
    VK_CMD_FILL_BUFFER = 2,  // vkCmdFillBuffer
    VK_CMD_COPY_BUFFER = 3,  // vkCmdCopyBuffer
    VK_CMD_DISPATCH = 4,     // vkCmdDispatch
    VK_CMD_BIND_VERTEX_BUFFER = 5,
    VK_CMD_BIND_RENDER_TARGET = 6, // bind the draw target (a bound framebuffer)
    VK_CMD_DRAW = 7,         // vkCmdDraw into the bound render target
};

struct MxVkCmdFillBuffer {
    MxVkCommandHeader header;
    mx_u64_t dest_arena_offset; // arena byte offset to fill from
    mx_u64_t offset;            // offset within that region
    mx_u64_t size;              // bytes to fill
    mx_u32_t value;             // 32-bit pattern
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkCmdFillBuffer) <= MX_VK_MAX_COMMAND_BYTES,
                 "fill-buffer record exceeds the max command size");
MX_STATIC_ASSERT(sizeof(struct MxVkCmdFillBuffer) == 48, "fill-buffer record size drift");

struct MxVkCmdCopyBuffer {
    MxVkCommandHeader header;
    mx_u64_t src_arena_offset;
    mx_u64_t dst_arena_offset;
    mx_u64_t size;
};
MX_STATIC_ASSERT(sizeof(struct MxVkCmdCopyBuffer) <= MX_VK_MAX_COMMAND_BYTES,
                 "copy-buffer record exceeds the max command size");
MX_STATIC_ASSERT(sizeof(struct MxVkCmdCopyBuffer) == 40, "copy-buffer record size drift");

struct MxVkCmdDispatch {
    MxVkCommandHeader header;
    mx_u32_t group_x;
    mx_u32_t group_y;
    mx_u32_t group_z;
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkCmdDispatch) <= MX_VK_MAX_COMMAND_BYTES,
                 "dispatch record exceeds the max command size");
MX_STATIC_ASSERT(sizeof(struct MxVkCmdDispatch) == 32, "dispatch record size drift");

struct MxVkCmdBindVertexBuffer {
    MxVkCommandHeader header;
    mx_u64_t arena_offset;
    mx_u64_t size_bytes;
};
MX_STATIC_ASSERT(sizeof(struct MxVkCmdBindVertexBuffer) <= MX_VK_MAX_COMMAND_BYTES,
                 "bind-vertex-buffer record exceeds the max command size");
MX_STATIC_ASSERT(sizeof(struct MxVkCmdBindVertexBuffer) == 32,
                 "bind-vertex-buffer record size drift");

// Binds a render target (the acquired swapchain image or any arena region) so a
// following DRAW knows where to rasterise: the transport mirror of a render
// pass / framebuffer bind.
struct MxVkCmdBindRenderTarget {
    MxVkCommandHeader header;
    mx_u64_t arena_offset;
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t stride; // bytes per row
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxVkCmdBindRenderTarget) <= MX_VK_MAX_COMMAND_BYTES,
                 "bind-render-target record exceeds the max command size");
MX_STATIC_ASSERT(sizeof(struct MxVkCmdBindRenderTarget) == 40,
                 "bind-render-target record size drift");

struct MxVkCmdDraw {
    MxVkCommandHeader header;
    mx_u32_t vertex_count;
    mx_u32_t instance_count;
    mx_u64_t color_xrgb8888; // packed XRGB8888 the draw writes into the target
};
MX_STATIC_ASSERT(sizeof(struct MxVkCmdDraw) <= MX_VK_MAX_COMMAND_BYTES,
                 "draw record exceeds the max command size");
MX_STATIC_ASSERT(sizeof(struct MxVkCmdDraw) == 32, "draw record size drift");

// The control-ring frame for this protocol is the shared ring slot verbatim,
// spelled per language exactly as protocols/gpu.h and gpu_service.h do.
#if defined(__cplusplus)
using MxVkFrame = MxRingSlot;
#else
typedef struct dango_ring_slot MxVkFrame;
#endif
MX_STATIC_ASSERT(sizeof(MxVkFrame) == 64, "Vulkan session frame must be one ring slot");

// --- Wire helpers (single home for slot encode/decode) ----------------------
//
// These copy byte-by-byte (no memcpy dependency, like the ring library) and are
// C/C++ compatible, so the native client, the GPU service broker and the guest
// dango-vkd all frame messages through the same two functions.

static inline void mx_vk_slot_encode(MxVkFrame *slot, mx_u32_t opcode, mx_u64_t seq,
                                     const void *payload, mx_u32_t bytes) {
    slot->type = opcode;
    slot->length = bytes;
    slot->seq = seq;
    const mx_u8_t *src = (const mx_u8_t *)payload;
    for (mx_u32_t i = 0; i < MX_VK_SESSION_INLINE_PAYLOAD; ++i) {
        slot->payload[i] = (payload && i < bytes) ? src[i] : (mx_u8_t)0;
    }
}

// Confirm `slot` is a request of `expected_opcode` with exactly `expected_bytes`
// of payload, then copy the payload out. Returns 0 on success, -1 otherwise.
static inline int mx_vk_slot_decode(const MxVkFrame *slot, mx_u32_t expected_opcode,
                                    mx_u32_t expected_bytes, void *out) {
    if (slot->type != expected_opcode || slot->length != expected_bytes ||
        expected_bytes > MX_VK_SESSION_INLINE_PAYLOAD) {
        return -1;
    }
    mx_u8_t *dst = (mx_u8_t *)out;
    for (mx_u32_t i = 0; i < expected_bytes; ++i) {
        dst[i] = slot->payload[i];
    }
    return 0;
}

// 1 when an opcode belongs to this protocol's base range.
static inline int mx_vk_is_opcode(mx_u32_t opcode) {
    return (opcode & 0xFFFFFF00u) == (MX_PROTOCOL_VULKAN_SESSION & 0xFFFFFF00u);
}

// --- Compile-time invariants ------------------------------------------------

#if defined(__cplusplus)
// Every opcode value is unique; enumerated in one place so a copy/paste
// duplicate or a renumbered opcode fails the build in every translation unit.
constexpr mx_u32_t kMxVkOpcodeValues[] = {
    VK_SESSION_OPEN,           VK_SESSION_OPEN_REPLY,
    VK_SESSION_CLOSE,          VK_SESSION_CLOSE_REPLY,
    VK_SESSION_ALLOC,          VK_SESSION_ALLOC_REPLY,
    VK_SESSION_FREE,           VK_SESSION_FREE_REPLY,
    VK_SESSION_SUBMIT,         VK_SESSION_SUBMIT_REPLY,
    VK_SESSION_SYNC_WAIT,      VK_SESSION_SYNC_WAIT_REPLY,
    VK_SESSION_SURFACE_CREATE, VK_SESSION_SURFACE_CREATE_REPLY,
    VK_SESSION_SURFACE_DESTROY, VK_SESSION_SURFACE_DESTROY_REPLY,
    VK_SESSION_SWAPCHAIN_CREATE, VK_SESSION_SWAPCHAIN_CREATE_REPLY,
    VK_SESSION_SWAPCHAIN_ACQUIRE, VK_SESSION_SWAPCHAIN_ACQUIRE_REPLY,
    VK_SESSION_SWAPCHAIN_PRESENT, VK_SESSION_SWAPCHAIN_PRESENT_REPLY,
    VK_SESSION_ERROR,
};

constexpr bool mx_vk_opcodes_unique() {
    constexpr mx_size_t count = sizeof(kMxVkOpcodeValues) / sizeof(kMxVkOpcodeValues[0]);
    for (mx_size_t i = 0; i < count; ++i) {
        for (mx_size_t j = i + 1; j < count; ++j) {
            if (kMxVkOpcodeValues[i] == kMxVkOpcodeValues[j]) {
                return false;
            }
        }
    }
    return true;
}
MX_STATIC_ASSERT(mx_vk_opcodes_unique(), "Vulkan session opcode values must be unique");

// Every command opcode is unique too, and never VK_CMD_NONE (0).
constexpr mx_u32_t kMxVkCommandValues[] = {
    VK_CMD_PIPELINE_BARRIER, VK_CMD_FILL_BUFFER, VK_CMD_COPY_BUFFER,
    VK_CMD_DISPATCH, VK_CMD_BIND_VERTEX_BUFFER, VK_CMD_BIND_RENDER_TARGET, VK_CMD_DRAW,
};

constexpr bool mx_vk_commands_unique() {
    constexpr mx_size_t count = sizeof(kMxVkCommandValues) / sizeof(kMxVkCommandValues[0]);
    for (mx_size_t i = 0; i < count; ++i) {
        if (kMxVkCommandValues[i] == VK_CMD_NONE) return false;
        for (mx_size_t j = i + 1; j < count; ++j) {
            if (kMxVkCommandValues[i] == kMxVkCommandValues[j]) return false;
        }
    }
    return true;
}
MX_STATIC_ASSERT(mx_vk_commands_unique(), "Vulkan command opcode values must be unique");
#endif

// The protocol base must not collide with the appliance control plane. (The
// distinctness from the native GPU *service* face, MX_PROTOCOL_GPU_SERVICE, is
// asserted where both headers are in scope: the host ABI probe.)
MX_STATIC_ASSERT(MX_PROTOCOL_VULKAN_SESSION != MX_PROTOCOL_GPU,
                 "the Vulkan session and appliance protocol ids must differ");

// The command region plus at least one minimum allocation must fit the default
// arena, or a default-sized session could never do useful work.
MX_STATIC_ASSERT(MX_VK_COMMAND_REGION_BYTES < MX_VK_SESSION_DEFAULT_ARENA_BYTES,
                 "the command region must leave room in the default arena");
MX_STATIC_ASSERT(MX_VK_COMMAND_REGION_BYTES < MX_VK_SESSION_MAX_ARENA_BYTES,
                 "the command region must fit the maximum arena");
MX_STATIC_ASSERT(MX_VK_SESSION_DEFAULT_ARENA_BYTES <= MX_VK_SESSION_MAX_ARENA_BYTES,
                 "the default arena must not exceed the maximum");
MX_STATIC_ASSERT(MX_VK_MAX_COMMAND_BYTES % MX_VK_COMMAND_ALIGNMENT == 0,
                 "the max command size must be record-aligned");
MX_STATIC_ASSERT(MX_VK_SESSION_ARENA_ALIGNMENT >= MX_VK_COMMAND_ALIGNMENT,
                 "the arena alignment must cover the command alignment");

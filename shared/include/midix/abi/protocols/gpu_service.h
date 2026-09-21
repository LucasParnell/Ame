#pragma once
// Midix GPU *service* protocol ABI — the Mochi-facing face of the Ame GPU
// appliance (`drv.gpu.linux`, spec §15.1/§15.2).
//
// This is the contract between a native Mochi client (io.framebuffer today) and
// the native GPU service whose server loop runs inside `dango.manager`. It is
// deliberately tiny and appliance-shaped:
//
//   * the client registers the shared window it renders into (REGISTER_STAGING),
//   * presents it (PRESENT) and learns whether the appliance is live (INFO),
//   * the service answers every request and completes a present asynchronously.
//
// It never exposes a DRM object, a virtio-gpu register, or the raw
// `protocols/gpu.h` opcode space: the service translates these requests into
// the Phase H/I GPU opcodes internally, and only `dango-gpud` maps an object id
// to a DRM handle (spec §13.1/§13.4). The service and its client share the
// `protocols/gpu.h` *value* types (pixel format, damage rectangle) so the two
// layers cannot drift; the opcodes stay distinct, in the 'GSV' protocol range.
//
// Transport envelope: MxRingSlot, exactly like every other Mochi service ring.
// `type` is the opcode, `length` bounds the inline payload (48 bytes), and `seq`
// is the envelope's running index (the producer stamps it on push, as
// everywhere in Mochi), so replies are matched by request order.

// The shared value types (MxGpuPixelFormat, MxGpuDamageRect, the 48-byte inline
// budget) and the MX_STATIC_ASSERT macro live in the appliance header, which is
// C/C++ compatible for exactly this reason (spec §13.1).
#include "midix/abi/protocols/gpu.h"

// The protocol base id (spec §15.1). Distinct from MX_PROTOCOL_GPU: this is the
// native service face, not the appliance control plane.
#define MX_PROTOCOL_GPU_SERVICE 0x47535600u // 'GSV\0'
#define MX_GPU_SERVICE_VERSION_MAJOR 1
#define MX_GPU_SERVICE_VERSION_MINOR 0

// The service reuses the appliance envelope verbatim; state it once.
#define MX_GPU_SERVICE_INLINE_PAYLOAD MX_GPU_INLINE_PAYLOAD

enum MxGpuServiceOpcode {
    // --- Native service surface (spec §15.2) ------------------------------
    GPU_SERVICE_INFO = 0x47535601u,                  // no payload -> INFO_REPLY
    GPU_SERVICE_INFO_REPLY = 0x47535602u,
    GPU_SERVICE_REGISTER_STAGING = 0x47535603u,      // RegisterStaging -> reply
    GPU_SERVICE_REGISTER_STAGING_REPLY = 0x47535604u,
    GPU_SERVICE_PRESENT = 0x47535605u,               // Present -> PRESENT_COMPLETE
    GPU_SERVICE_PRESENT_COMPLETE = 0x47535606u,
    // Rejected request; `opcode` names the request the service refused.
    GPU_SERVICE_ERROR = 0x475356FFu,
};

enum MxGpuServiceFlags {
    MX_GPU_SERVICE_FLAG_NONE = 0,
    // Advertised by INFO_REPLY: the appliance is up and this service will
    // accept REGISTER_STAGING and PRESENT. Absent => the client must keep its
    // local (ramfb) path and never depend on the appliance.
    MX_GPU_SERVICE_FLAG_LIVE = 1u << 0,
};

enum MxGpuServicePresentFlags {
    MX_GPU_SERVICE_PRESENT_FLAG_NONE = 0,
    // The damage field is ignored and the whole window is presented. A client
    // sets this for the first present after it attaches, so a stale appliance
    // scanout can never survive the handshake.
    MX_GPU_SERVICE_PRESENT_FLAG_FULL_FRAME = 1u << 0,
};

enum MxGpuServiceBufferFlags {
    MX_GPU_SERVICE_BUFFER_FLAG_NONE = 0,
    MX_GPU_SERVICE_BUFFER_FLAG_STAGING = 1u << 0, // the Dango shared staging window
};

// --- GPU_SERVICE_INFO: liveness + scanout geometry --------------------------
//
// The request carries no payload. The reply states whether the appliance is
// live and, when it is, the geometry the service scans out; the client uses it
// to confirm the window it renders matches before it starts presenting.
// `mx_status_t` is a 64-bit value, so every struct below fixes its field order
// explicitly (u64 fields first, then u32s, with an explicit reserved tail) so no
// field is silently moved by implicit padding and the wire size is exact.
struct MxGpuServiceInfoReply {
    mx_status_t status;
    mx_u64_t boot_generation;
    mx_u32_t flags;         // MxGpuServiceFlags
    mx_u32_t pixel_format;  // MxGpuPixelFormat
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t stride;        // bytes per row
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceInfoReply) <= MX_GPU_SERVICE_INLINE_PAYLOAD,
                 "GPU service INFO reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceInfoReply) == 40, "INFO reply size drift");

// --- GPU_SERVICE_REGISTER_STAGING -------------------------------------------
//
// The client declares the window it renders into and the appliance staging
// binding that window feeds (both from the shared Dango binding). The service
// validates them against its own binding and mints the opaque buffer token the
// client echoes on every present; a client built against a different layout is
// rejected rather than silently mis-presented.
struct MxGpuServiceRegisterStagingRequest {
    mx_u64_t staging_pa;    // guest PA of the appliance staging window
    mx_u64_t staging_size;  // bytes
    mx_u32_t width;
    mx_u32_t height;
    mx_u32_t stride;        // bytes per row
    mx_u32_t pixel_format;  // MxGpuPixelFormat
    mx_u32_t flags;         // MxGpuServiceBufferFlags
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceRegisterStagingRequest) <= MX_GPU_SERVICE_INLINE_PAYLOAD,
                 "REGISTER_STAGING request must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceRegisterStagingRequest) == 40,
                 "REGISTER_STAGING request size drift");

struct MxGpuServiceRegisterStagingReply {
    mx_status_t status;
    mx_u64_t buffer_id;    // opaque, boot-generation-scoped buffer token
    mx_u32_t flags;        // MxGpuServiceBufferFlags echo
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceRegisterStagingReply) <= MX_GPU_SERVICE_INLINE_PAYLOAD,
                 "REGISTER_STAGING reply must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceRegisterStagingReply) == 24,
                 "REGISTER_STAGING reply size drift");

// --- GPU_SERVICE_PRESENT ----------------------------------------------------
struct MxGpuServicePresentRequest {
    mx_u64_t buffer_id;    // token from REGISTER_STAGING
    mx_u32_t flags;        // MxGpuServicePresentFlags
    mx_u32_t damage_count; // 1 => damage is valid; 0 => nothing to present unless FULL_FRAME
    MxGpuDamageRect damage; // valid when damage_count == 1
};
MX_STATIC_ASSERT(sizeof(struct MxGpuServicePresentRequest) <= MX_GPU_SERVICE_INLINE_PAYLOAD,
                 "PRESENT request must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxGpuServicePresentRequest) == 32, "PRESENT request size drift");

// Completion of one present. The service answers every PRESENT it accepts, so a
// client that queues N presents receives N completions in order; the envelope
// `seq` is the reply ring's running index, as on every Mochi ring.
struct MxGpuServicePresentComplete {
    mx_status_t status;
    mx_u64_t buffer_id;
    mx_u64_t frame_index;  // monotonic count of completed presents
    mx_u32_t flags;        // MxGpuServicePresentFlags echo
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxGpuServicePresentComplete) <= MX_GPU_SERVICE_INLINE_PAYLOAD,
                 "PRESENT_COMPLETE must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxGpuServicePresentComplete) == 32, "PRESENT_COMPLETE size drift");

// --- GPU_SERVICE_ERROR ------------------------------------------------------
struct MxGpuServiceError {
    mx_status_t status;
    mx_u64_t detail; // request-specific (0 when none)
    mx_u32_t opcode; // the request opcode the service refused
    mx_u32_t reserved0;
};
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceError) <= MX_GPU_SERVICE_INLINE_PAYLOAD,
                 "GPU service error must fit one payload");
MX_STATIC_ASSERT(sizeof(struct MxGpuServiceError) == 24, "GPU service error size drift");

// The control-ring frame for this protocol is the shared ring slot verbatim.
// (C has no struct-tag elision or `using`, so the alias is spelled per language,
// exactly as protocols/gpu.h does for MxGpuFrame.)
#if defined(__cplusplus)
using MxGpuServiceFrame = MxRingSlot;
#else
typedef struct dango_ring_slot MxGpuServiceFrame;
#endif
MX_STATIC_ASSERT(sizeof(MxGpuServiceFrame) == 64, "GPU service frame must be one ring slot");
MX_STATIC_ASSERT(MX_GPU_SERVICE_INLINE_PAYLOAD == 48, "GPU service payload budget changed");

// --- Compile-time invariants ------------------------------------------------

#if defined(__cplusplus)
// Every opcode value is unique; enumerated in one place so a copy/paste
// duplicate fails the build in every translation unit.
constexpr mx_u32_t kMxGpuServiceOpcodeValues[] = {
    GPU_SERVICE_INFO,           GPU_SERVICE_INFO_REPLY,
    GPU_SERVICE_REGISTER_STAGING, GPU_SERVICE_REGISTER_STAGING_REPLY,
    GPU_SERVICE_PRESENT,        GPU_SERVICE_PRESENT_COMPLETE,
    GPU_SERVICE_ERROR,
};

constexpr bool mx_gpu_service_opcodes_unique() {
    constexpr mx_size_t count =
        sizeof(kMxGpuServiceOpcodeValues) / sizeof(kMxGpuServiceOpcodeValues[0]);
    for (mx_size_t i = 0; i < count; ++i) {
        for (mx_size_t j = i + 1; j < count; ++j) {
            if (kMxGpuServiceOpcodeValues[i] == kMxGpuServiceOpcodeValues[j]) {
                return false;
            }
        }
    }
    return true;
}
MX_STATIC_ASSERT(mx_gpu_service_opcodes_unique(), "GPU service opcode values must be unique");
#endif

// The service protocol lives in its own base range; it must not collide with
// the appliance control-plane base.
MX_STATIC_ASSERT(MX_PROTOCOL_GPU_SERVICE != MX_PROTOCOL_GPU,
                 "the GPU service and appliance protocol ids must differ");

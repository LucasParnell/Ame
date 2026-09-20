// tests/gpu_proto_probe.cc — host regression for the Midix GPU protocol and the
// Dango paravirtual channel ABI headers.
//
// Header-only: it includes both protocol headers, then pins their sizes, field
// offsets, opcode uniqueness, and the PSCI-disjoint HVC function ids. The
// headers carry the same invariants as static_assert; this probe proves they do
// so while compiling clean under -Wall -Wextra -Werror and that the recorded
// offsets/values agree with the wire contract.

#include "midix/abi/protocols/dango_channel.h"
#include "midix/abi/protocols/gpu.h"

#include <cassert>
#include <cstddef>
#include <cstdio>

namespace {

// --- GPU: struct sizes ------------------------------------------------------

static_assert(sizeof(MxGpuHello) == 24);
static_assert(sizeof(MxGpuHelloReply) == 32);
static_assert(sizeof(MxGpuEnumOutputsRequest) == 24);
static_assert(sizeof(MxGpuEnumOutputsReply) == 48);
static_assert(sizeof(MxGpuGetModesRequest) == 24);
static_assert(sizeof(MxGpuGetModesReply) == 48);
static_assert(sizeof(MxGpuBufferRegisterRequest) == 40);
static_assert(sizeof(MxGpuBufferRegisterReply) == 32);
static_assert(sizeof(MxGpuBufferUnregisterRequest) == 16);
static_assert(sizeof(MxGpuBufferUnregisterReply) == 16);
static_assert(sizeof(MxGpuSetModeRequest) == 40);
static_assert(sizeof(MxGpuSetModeReply) == 24);
static_assert(sizeof(MxGpuPresentRequest) == 48);
static_assert(sizeof(MxGpuPresentReply) == 32);
static_assert(sizeof(MxGpuPresentComplete) == 32);
static_assert(sizeof(MxGpuError) == 48);
static_assert(sizeof(MxGpuDamageRect) == 16);
static_assert(sizeof(MxGpuFrame) == 64);

// --- GPU: field offsets on the version-negotiation struct -------------------

static_assert(offsetof(MxGpuHello, major) == 0);
static_assert(offsetof(MxGpuHello, minor) == 2);
static_assert(offsetof(MxGpuHello, flags) == 4);
static_assert(offsetof(MxGpuHello, feature_bits) == 8);
static_assert(offsetof(MxGpuHello, boot_generation) == 16);

// --- GPU: object-id + boot-generation placement in representative payloads --

static_assert(offsetof(MxGpuEnumOutputsRequest, device_id) == 0);
static_assert(offsetof(MxGpuEnumOutputsRequest, boot_generation) == 8);
static_assert(offsetof(MxGpuEnumOutputsReply, output_id) == 8);
static_assert(offsetof(MxGpuSetModeRequest, output_id) == 0);
static_assert(offsetof(MxGpuSetModeRequest, mode_id) == 8);
static_assert(offsetof(MxGpuSetModeRequest, buffer_id) == 16);
static_assert(offsetof(MxGpuSetModeRequest, boot_generation) == 24);
static_assert(offsetof(MxGpuPresentRequest, buffer_id) == 0);
static_assert(offsetof(MxGpuPresentRequest, framebuffer_id) == 8);
static_assert(offsetof(MxGpuPresentRequest, boot_generation) == 16);
static_assert(offsetof(MxGpuPresentRequest, damage) == 32);
static_assert(offsetof(MxGpuError, object_id) == 8);
static_assert(offsetof(MxGpuError, boot_generation) == 16);
static_assert(offsetof(MxGpuError, opcode) == 32);

// --- GPU: every inline payload fits the 48-byte slot budget -----------------

static_assert(sizeof(MxGpuHello) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuHelloReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuEnumOutputsRequest) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuEnumOutputsReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuGetModesRequest) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuGetModesReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuBufferRegisterRequest) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuBufferRegisterReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuBufferUnregisterRequest) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuBufferUnregisterReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuSetModeRequest) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuSetModeReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuPresentRequest) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuPresentReply) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuPresentComplete) <= MX_RING_INLINE_PAYLOAD);
static_assert(sizeof(MxGpuError) <= MX_RING_INLINE_PAYLOAD);

// --- Dango channel: struct sizes and layout ---------------------------------

static_assert(sizeof(MxDangoChannelDesc) == 64);
static_assert(sizeof(MxDangoChannelQueryRequest) == 8);
static_assert(sizeof(MxDangoChannelQueryReply) == 40);
static_assert(sizeof(MxDangoChannelNotify) == 16);
static_assert(sizeof(MxDangoChannelArm) == 16);
static_assert(sizeof(MxDangoChannelFrame) == 64);
static_assert(offsetof(MxDangoChannelQueryReply, status) == 0);
static_assert(offsetof(MxDangoChannelQueryReply, boot_generation) == 24);
static_assert(offsetof(MxDangoChannelDesc, boot_generation) == 16);
static_assert(offsetof(MxDangoChannelDesc, host_to_guest_ring_pa) == 24);
static_assert(offsetof(MxDangoChannelDesc, guest_to_host_ring_pa) == 32);

constexpr mx_u32_t kDangoOpcodeValues[] = {
    DANGO_CHANNEL_QUERY,
    DANGO_CHANNEL_QUERY_REPLY,
    DANGO_CHANNEL_NOTIFY,
    DANGO_CHANNEL_ERROR,
};

} // namespace

int main() {
    // (1) Every GPU opcode value is unique.
    constexpr mx_size_t count = sizeof(kMxGpuOpcodeValues) / sizeof(kMxGpuOpcodeValues[0]);
    assert(count == 21);
    for (mx_size_t i = 0; i < count; ++i) {
        for (mx_size_t j = i + 1; j < count; ++j) {
            assert(kMxGpuOpcodeValues[i] != kMxGpuOpcodeValues[j]);
        }
    }

    // (2) Every Dango channel opcode value is unique and distinct from GPU ones.
    constexpr mx_size_t dango_count = sizeof(kDangoOpcodeValues) / sizeof(kDangoOpcodeValues[0]);
    for (mx_size_t i = 0; i < dango_count; ++i) {
        for (mx_size_t j = i + 1; j < dango_count; ++j) {
            assert(kDangoOpcodeValues[i] != kDangoOpcodeValues[j]);
        }
        for (mx_size_t j = 0; j < count; ++j) {
            assert(kDangoOpcodeValues[i] != kMxGpuOpcodeValues[j]);
        }
    }

    // (3) The bootstrap opcodes keep their exact wire values.
    assert(GPU_HELLO == 0x47505501u);
    assert(GPU_ENUM_OUTPUTS == 0x47505502u);
    assert(GPU_GET_MODES == 0x47505503u);
    assert(GPU_BUFFER_REGISTER == 0x47505504u);
    assert(GPU_BUFFER_UNREGISTER == 0x47505505u);
    assert(GPU_SET_MODE == 0x47505506u);
    assert(GPU_PRESENT == 0x47505507u);
    assert(GPU_PRESENT_COMPLETE == 0x47505508u);
    assert(GPU_ERROR == 0x47505509u);

    // (4) The future opcodes live in the reserved block, ahead of the bootstrap
    //     set, and the whole set stays below the reserved limit.
    assert(GPU_ERROR < MX_GPU_OPCODE_FUTURE_BASE);
    assert(GPU_BUFFER_CREATE >= MX_GPU_OPCODE_FUTURE_BASE);
    assert(GPU_GET_PROPERTIES <= MX_GPU_OPCODE_FUTURE_LIMIT);

    // (5) Core object-type ids are non-zero and unique.
    constexpr mx_u32_t kObjectTypes[] = {
        MX_GPU_OBJECT_DEVICE,      MX_GPU_OBJECT_OUTPUT,  MX_GPU_OBJECT_MODE,  MX_GPU_OBJECT_BUFFER,
        MX_GPU_OBJECT_FRAMEBUFFER, MX_GPU_OBJECT_CONTEXT, MX_GPU_OBJECT_FENCE,
    };
    constexpr mx_size_t obj_count = sizeof(kObjectTypes) / sizeof(kObjectTypes[0]);
    for (mx_size_t i = 0; i < obj_count; ++i) {
        assert(kObjectTypes[i] != 0);
        for (mx_size_t j = i + 1; j < obj_count; ++j) {
            assert(kObjectTypes[i] != kObjectTypes[j]);
        }
    }

    // (6) Feature bits are single, unique bits.
    constexpr mx_u64_t kFeatureBits[] = {
        MX_GPU_FEATURE_STAGING_BUFFER, MX_GPU_FEATURE_ASYNC_PRESENT, MX_GPU_FEATURE_DAMAGE_RECTS,
        MX_GPU_FEATURE_CURSOR,         MX_GPU_FEATURE_ATOMIC_COMMIT, MX_GPU_FEATURE_BUFFER_EXPORT,
        MX_GPU_FEATURE_RENDER_CONTEXT, MX_GPU_FEATURE_REMOTE_VULKAN,
    };
    constexpr mx_size_t feature_count = sizeof(kFeatureBits) / sizeof(kFeatureBits[0]);
    for (mx_size_t i = 0; i < feature_count; ++i) {
        assert(kFeatureBits[i] != 0);
        assert((kFeatureBits[i] & (kFeatureBits[i] - 1)) == 0);
        for (mx_size_t j = i + 1; j < feature_count; ++j) {
            assert(kFeatureBits[i] != kFeatureBits[j]);
        }
    }

    // (7) The Dango HVC ids are in the vendor hypervisor range and cannot
    //     collide with PSCI (neither the 32-bit nor the 64-bit range).
    assert(HVC_DANGO_CHANNEL_NOTIFY == 0xC6000001u);
    assert(HVC_DANGO_CHANNEL_ARM == 0xC6000002u);
    assert(HVC_DANGO_CHANNEL_NOTIFY != HVC_DANGO_CHANNEL_ARM);
    assert(mx_dango_hvc_avoid_psci(HVC_DANGO_CHANNEL_NOTIFY));
    assert(mx_dango_hvc_avoid_psci(HVC_DANGO_CHANNEL_ARM));
    assert(!mx_dango_hvc_avoid_psci(0x84000000u)); // PSCI 32-bit base
    assert(!mx_dango_hvc_avoid_psci(0xC4000003u)); // PSCI CPU_ON (64-bit)
    assert(mx_dango_hvc_in_vendor_range(HVC_DANGO_CHANNEL_NOTIFY));
    assert(mx_dango_hvc_in_vendor_range(HVC_DANGO_CHANNEL_ARM));

    // (8) The channel descriptor addresses a Midix-contract ring.
    assert(MX_DANGO_CHANNEL_SLOTS == (1u << MX_DANGO_CHANNEL_RING_ORDER_DEFAULT));
    assert(MX_DANGO_CHANNEL_MAGIC != 0);

    std::printf("gpu/dango protocol ABI probe ok\n");
    return 0;
}

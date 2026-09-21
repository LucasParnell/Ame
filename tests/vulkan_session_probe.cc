// tests/vulkan_session_probe.cc — host regression for the Midix remote Vulkan
// session transport ABI header (shared/include/midix/abi/protocols/vulkan_session.h).
//
// Header-only: it pins every message size and field offset, the opcode and
// command-value uniqueness, the object-id layout, and the quota invariants. The
// header carries the same facts as static_assert; this probe proves they hold
// while compiling clean under -Wall -Wextra -Werror, so a size or offset drift,
// a duplicated opcode or a bad id bit-layout fails the host test.

#include "midix/abi/protocols/vulkan_session.h"
#include "midix/abi/protocols/gpu_service.h" // for the distinctness check only

#include <cassert>
#include <cstddef>
#include <cstdio>

namespace {

// --- Message sizes ----------------------------------------------------------

static_assert(sizeof(MxVkSessionOpenRequest) == 32);
static_assert(sizeof(MxVkSessionOpenReply) == 48);
static_assert(sizeof(MxVkSessionCloseRequest) == 16);
static_assert(sizeof(MxVkSessionCloseReply) == 24);
static_assert(sizeof(MxVkSessionAllocRequest) == 32);
static_assert(sizeof(MxVkSessionAllocReply) == 48);
static_assert(sizeof(MxVkSessionFreeRequest) == 16);
static_assert(sizeof(MxVkSessionFreeReply) == 16);
static_assert(sizeof(MxVkSessionSubmitRequest) == 32);
static_assert(sizeof(MxVkSessionSubmitReply) == 40);
static_assert(sizeof(MxVkSessionSyncWaitRequest) == 32);
static_assert(sizeof(MxVkSessionSyncWaitReply) == 24);
static_assert(sizeof(MxVkSurfaceCreateRequest) == 24);
static_assert(sizeof(MxVkSurfaceCreateReply) == 32);
static_assert(sizeof(MxVkSwapchainCreateRequest) == 32);
static_assert(sizeof(MxVkSwapchainCreateReply) == 32);
static_assert(sizeof(MxVkSwapchainAcquireRequest) == 32);
static_assert(sizeof(MxVkSwapchainAcquireReply) == 40);
static_assert(sizeof(MxVkSwapchainPresentRequest) == 40);
static_assert(sizeof(MxVkSwapchainPresentReply) == 32);
static_assert(sizeof(MxVkSessionError) == 32);
static_assert(sizeof(MxVkCommandHeader) == 16);
static_assert(sizeof(MxVkCmdFillBuffer) == 48);
static_assert(sizeof(MxVkCmdCopyBuffer) == 40);
static_assert(sizeof(MxVkCmdDispatch) == 32);
static_assert(sizeof(MxVkCmdBindVertexBuffer) == 32);
static_assert(sizeof(MxVkCmdBindRenderTarget) == 40);
static_assert(sizeof(MxVkCmdDraw) == 32);
static_assert(sizeof(MxVkFrame) == 64);

// --- Every inline message fits the 48-byte slot budget ----------------------

static_assert(sizeof(MxVkSessionOpenRequest) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(sizeof(MxVkSessionOpenReply) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(sizeof(MxVkSessionAllocReply) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(sizeof(MxVkSessionSubmitRequest) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(sizeof(MxVkSessionSubmitReply) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(sizeof(MxVkSwapchainPresentRequest) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(sizeof(MxVkSwapchainAcquireReply) <= MX_VK_SESSION_INLINE_PAYLOAD);
static_assert(MX_VK_SESSION_INLINE_PAYLOAD == 48);

// --- Field offsets on the messages whose layout is load-bearing -------------

static_assert(offsetof(MxVkSessionOpenRequest, arena_bytes) == 8);
static_assert(offsetof(MxVkSessionOpenRequest, max_objects) == 16);
static_assert(offsetof(MxVkSessionOpenRequest, client_id) == 24);
static_assert(offsetof(MxVkSessionOpenReply, session_id) == 8);
static_assert(offsetof(MxVkSessionOpenReply, boot_generation) == 16);
static_assert(offsetof(MxVkSessionOpenReply, arena_id) == 24);
static_assert(offsetof(MxVkSessionOpenReply, arena_bytes) == 32);
static_assert(offsetof(MxVkSessionAllocReply, memory_id) == 8);
static_assert(offsetof(MxVkSessionAllocReply, arena_offset) == 24);
static_assert(offsetof(MxVkSessionSubmitRequest, command_offset) == 8);
static_assert(offsetof(MxVkSessionSubmitRequest, sync_id) == 16);
static_assert(offsetof(MxVkSessionSubmitRequest, command_bytes) == 24);
static_assert(offsetof(MxVkSwapchainPresentRequest, damage) == 24);
static_assert(offsetof(MxVkSwapchainAcquireReply, arena_offset) == 24);
static_assert(offsetof(MxVkCmdFillBuffer, dest_arena_offset) == 16);
static_assert(offsetof(MxVkCmdCopyBuffer, size) == 32);
static_assert(offsetof(MxVkCmdBindRenderTarget, width) == 24);
static_assert(offsetof(MxVkCmdDraw, color_xrgb8888) == 24);

// --- Object-id layout -------------------------------------------------------

static_assert(MX_VK_OBJECT_GENERATION_SHIFT == 32);
static_assert(MX_VK_OBJECT_MAX_INDEX == 0xFFFull);
static_assert(MX_VK_SESSION_MAX_OBJECTS <= MX_VK_OBJECT_MAX_INDEX);

// --- Quota / region invariants ----------------------------------------------

static_assert(MX_VK_COMMAND_REGION_BYTES < MX_VK_SESSION_DEFAULT_ARENA_BYTES);
static_assert(MX_VK_SESSION_DEFAULT_ARENA_BYTES <= MX_VK_SESSION_MAX_ARENA_BYTES);
static_assert(MX_VK_MAX_COMMAND_BYTES % MX_VK_COMMAND_ALIGNMENT == 0);
static_assert(MX_VK_SESSION_SLOTS == (1u << MX_VK_SESSION_RING_ORDER));

} // namespace

int main() {
    // (1) Every session opcode value is unique and lands in the protocol range.
    constexpr mx_size_t count = sizeof(kMxVkOpcodeValues) / sizeof(kMxVkOpcodeValues[0]);
    assert(count == 23);
    for (mx_size_t i = 0; i < count; ++i) {
        assert(mx_vk_is_opcode(kMxVkOpcodeValues[i]));
        for (mx_size_t j = i + 1; j < count; ++j) {
            assert(kMxVkOpcodeValues[i] != kMxVkOpcodeValues[j]);
        }
    }
    // The GPU and GPU-service opcodes are not Vulkan-session opcodes.
    assert(!mx_vk_is_opcode(GPU_HELLO));
    assert(!mx_vk_is_opcode(GPU_SERVICE_INFO));

    // (2) The base ids are distinct and the version is fixed.
    assert(MX_PROTOCOL_VULKAN_SESSION != MX_PROTOCOL_GPU);
    assert(MX_PROTOCOL_VULKAN_SESSION != MX_PROTOCOL_GPU_SERVICE);
    assert(VK_SESSION_OPEN == 0x564B5301u);
    assert(VK_SESSION_SWAPCHAIN_PRESENT_REPLY == 0x564B5316u);
    assert(VK_SESSION_ERROR == 0x564B53FFu);
    assert(MX_VK_SESSION_VERSION_MAJOR == 1 && MX_VK_SESSION_VERSION_MINOR == 0);

    // (3) Every command opcode is unique and non-zero.
    constexpr mx_size_t cmd_count = sizeof(kMxVkCommandValues) / sizeof(kMxVkCommandValues[0]);
    for (mx_size_t i = 0; i < cmd_count; ++i) {
        assert(kMxVkCommandValues[i] != VK_CMD_NONE);
        for (mx_size_t j = i + 1; j < cmd_count; ++j) {
            assert(kMxVkCommandValues[i] != kMxVkCommandValues[j]);
        }
    }

    // (4) The opaque-object id round-trips through make/check.
    constexpr mx_u64_t gen = 0x1234ABCDull;
    const mx_u64_t mem_id = mx_vk_object_id_make(gen, MX_VK_OBJECT_MEMORY, 2, 7);
    mx_u32_t index = 0;
    assert(mx_vk_object_id_check(mem_id, MX_VK_OBJECT_MEMORY, gen, 2, &index) == 0 && index == 7);
    // A wrong type, a wrong session, a wrong generation and a null id all fail.
    assert(mx_vk_object_id_check(mem_id, MX_VK_OBJECT_SURFACE, gen, 2, &index) != 0);
    assert(mx_vk_object_id_check(mem_id, MX_VK_OBJECT_MEMORY, gen, 3, &index) != 0);
    assert(mx_vk_object_id_check(mem_id, MX_VK_OBJECT_MEMORY, gen + 1, 2, &index) != 0);
    assert(mx_vk_object_id_check(0, MX_VK_OBJECT_MEMORY, gen, 2, &index) != 0);

    // (5) The wire helpers frame and validate a slot byte-exactly.
    MxVkFrame frame{};
    MxVkSessionAllocRequest request{};
    request.session_id = 0xDEADBEEFull;
    request.size = 4096;
    request.memory_type = MX_VK_MEMORY_TYPE_HOST_VISIBLE;
    mx_vk_slot_encode(&frame, VK_SESSION_ALLOC, 42, &request, sizeof(request));
    assert(frame.type == VK_SESSION_ALLOC && frame.length == sizeof(request) && frame.seq == 42);
    MxVkSessionAllocRequest decoded{};
    assert(mx_vk_slot_decode(&frame, VK_SESSION_ALLOC, sizeof(decoded), &decoded) == 0);
    assert(decoded.session_id == request.session_id && decoded.size == request.size);
    // A wrong opcode or a wrong length is refused, never partially accepted.
    assert(mx_vk_slot_decode(&frame, VK_SESSION_FREE, sizeof(decoded), &decoded) != 0);
    assert(mx_vk_slot_decode(&frame, VK_SESSION_ALLOC, sizeof(decoded) - 8, &decoded) != 0);

    std::printf("vulkan session ABI probe ok\n");
    return 0;
}

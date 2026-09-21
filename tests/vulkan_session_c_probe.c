/* tests/vulkan_session_c_probe.c — compile-time proof that the Vulkan session
 * ABI header is consumable from C, exactly as the guest dango-vkd build uses it.
 *
 * The header is C/C++ compatible by construction (it selects the Dango ring view
 * under C and the Midix ring view under C++). This probe includes it as C and
 * pins the same wire facts the C++ probe does, so a C-only regression (a missing
 * typedef, a C++-only construct leaking into the wire structs) fails the host
 * test. It is built freestanding-style: no libc beyond stdint is used. */

#include <stddef.h>
#include <stdint.h>

#include "midix/abi/vulkan_commands.h"

_Static_assert(sizeof(struct MxVkSessionOpenRequest) == 32, "open request size");
_Static_assert(sizeof(struct MxVkSessionOpenReply) == 48, "open reply size");
_Static_assert(sizeof(struct MxVkSessionAllocReply) == 48, "alloc reply size");
_Static_assert(sizeof(struct MxVkSessionSubmitRequest) == 32, "submit request size");
_Static_assert(sizeof(struct MxVkSwapchainPresentRequest) == 40, "present request size");
_Static_assert(sizeof(struct MxVkCmdCopyBuffer) == 40, "copy-buffer record size");
_Static_assert(sizeof(struct MxVkCmdBindRenderTarget) == 40, "bind-render-target record size");
_Static_assert(sizeof(struct MxVkCmdDraw) == 32, "draw record size");
_Static_assert(sizeof(MxVkFrame) == 64, "frame is one ring slot");
_Static_assert(sizeof(MxVkCommandHeader) == 16, "command header size");
_Static_assert(MX_VK_SESSION_INLINE_PAYLOAD == 48, "inline payload budget");
_Static_assert(MX_VK_SESSION_SLOTS == (1u << MX_VK_SESSION_RING_ORDER), "slot count");

int mx_vk_c_translation_unit_is_valid(void);

int mx_vk_c_translation_unit_is_valid(void) {
    MxVkFrame frame;
    struct MxVkSessionSubmitRequest submit;
    submit.session_id = 1;
    submit.command_offset = 0;
    submit.sync_id = 9;
    submit.command_bytes = 16;
    submit.flags = MX_VK_SUBMIT_FLAG_SIGNAL;
    mx_vk_slot_encode(&frame, VK_SESSION_SUBMIT, 1, &submit, (mx_u32_t)sizeof(submit));
    if (frame.type != VK_SESSION_SUBMIT || frame.length != sizeof(submit)) {
        return 0;
    }

    struct MxVkSessionSubmitRequest decoded;
    if (mx_vk_slot_decode(&frame, VK_SESSION_SUBMIT, (mx_u32_t)sizeof(decoded), &decoded) != 0) {
        return 0;
    }
    if (decoded.sync_id != 9 || decoded.command_bytes != 16) {
        return 0;
    }

    /* The id layout round-trips in C exactly as it does in C++. */
    const uint64_t id = mx_vk_object_id_make(0x55, MX_VK_OBJECT_MEMORY, 1, 3);
    unsigned int index = 0;
    if (mx_vk_object_id_check(id, MX_VK_OBJECT_MEMORY, 0x55, 1, &index) != 0 || index != 3) {
        return 0;
    }
    if (mx_vk_object_id_check(id, MX_VK_OBJECT_MEMORY, 0x55, 2, &index) == 0) {
        return 0;
    }
    return 1;
}

/* Decode unaligned input and reject every truncated prefix without reading
 * outside it. The host test executes this under ASan/UBSan as well as compiling
 * the C ABI assertions above. */
static int commands_are_bounded(void) {
    MxVkCmdFillBuffer fill = {0};
    fill.header.opcode = VK_CMD_FILL_BUFFER;
    fill.header.size = sizeof(fill);
    fill.dest_arena_offset = MX_VK_COMMAND_REGION_BYTES;
    fill.size = 4;
    fill.value = 0x12345678;
    uint8_t unaligned[sizeof(fill) + 1];
    const uint8_t* source = (const uint8_t*)&fill;
    for (unsigned i = 0; i < sizeof(fill); ++i) {
        unaligned[i + 1] = source[i];
    }
    MxVkCommand command;
    for (unsigned bytes = 0; bytes < sizeof(fill); ++bytes) {
        if (!mx_vk_command_decode(unaligned + 1, bytes, &command)) {
            return 0;
        }
    }
    if (mx_vk_command_decode(unaligned + 1, sizeof(fill), &command) ||
        command.fill.value != fill.value) {
        return 0;
    }
    if (!mx_vk_data_range_valid(fill.dest_arena_offset, fill.size,
                                MX_VK_SESSION_DEFAULT_ARENA_BYTES) ||
        mx_vk_data_range_valid(UINT64_MAX, 4, MX_VK_SESSION_DEFAULT_ARENA_BYTES) ||
        mx_vk_data_range_valid(fill.dest_arena_offset, UINT64_MAX,
                               MX_VK_SESSION_DEFAULT_ARENA_BYTES) ||
        mx_vk_data_range_valid(0, 4, MX_VK_SESSION_DEFAULT_ARENA_BYTES)) {
        return 0;
    }
    fill.header.flags = 1;
    if (!mx_vk_command_decode((const uint8_t*)&fill, sizeof(fill), &command)) {
        return 0;
    }
    fill.header.flags = 0;
    fill.reserved0 = 1;
    if (!mx_vk_command_decode((const uint8_t*)&fill, sizeof(fill), &command)) {
        return 0;
    }
    fill.reserved0 = 0;
    fill.header.size = sizeof(MxVkCommandHeader);
    if (!mx_vk_command_decode((const uint8_t*)&fill, sizeof(fill), &command)) {
        return 0;
    }
    fill.header.opcode = UINT32_MAX;
    if (!mx_vk_command_decode((const uint8_t*)&fill, sizeof(fill), &command)) {
        return 0;
    }
    return 1;
}

int main(void) {
    return mx_vk_c_translation_unit_is_valid() && commands_are_bounded() ? 0 : 1;
}

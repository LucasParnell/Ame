/* Real lavapipe regression, runnable in a container chroot or as Linux PID 1.
 * Checks device/host-visible allocation, dependent transfers, fences/readback,
 * malformed command atomicity, capacity bounds and startup failure cleanup. */
#define _POSIX_C_SOURCE 200809L
#include "vkd_runtime.h"
#include "vkd_vulkan.h"
#include "midix/abi/vulkan_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t arena[MX_VK_SESSION_DEFAULT_ARENA_BYTES];
static uint8_t before[sizeof(arena)];
#define CHECK(test)                                                                                \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            printf("vkd: TEST FAILED line=%u: %s\n", (unsigned)__LINE__, #test);                   \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

static MxVkCmdFillBuffer fill_command(void) {
    MxVkCmdFillBuffer command = {0};
    command.header.opcode = VK_CMD_FILL_BUFFER;
    command.header.size = sizeof(command);
    command.dest_arena_offset = MX_VK_COMMAND_REGION_BYTES;
    command.offset = 16;
    command.size = 64;
    command.value = 0x12345678;
    return command;
}

static int execute(uint32_t size) {
    return vkd_vulkan_execute(arena, sizeof(arena), 0, size);
}

static int rejected(const void* command, uint32_t size) {
    memcpy(arena, command, size);
    memcpy(before, arena, sizeof(arena));
    return execute(size) != 0 && memcmp(before, arena, sizeof(arena)) == 0;
}

int main(int argc, char** argv) {
    CHECK(vkd_runtime_init() == 0);
    if (argc == 2 && strcmp(argv[1], "--missing-icd") == 0) {
        CHECK(setenv("VK_DRIVER_FILES", "/absent-icd.json", 1) == 0);
        for (unsigned i = 0; i < 16; ++i) {
            CHECK(vkd_vulkan_init() != 0 && !vkd_vulkan_available());
            vkd_vulkan_shutdown();
        }
        puts("vkd: missing ICD refused");
        return vkd_runtime_exit(0);
    }
    CHECK(argc == 1);
    CHECK(vkd_vulkan_init() == 0 && vkd_vulkan_available());
    CHECK(vkd_vulkan_init() == 0); /* idempotent */
    puts("vkd: real Vulkan device ready");
    memset(arena, 0xa5, sizeof(arena));
    MxVkCmdFillBuffer fill = fill_command();
    MxVkCmdCopyBuffer copy = {0};
    copy.header.opcode = VK_CMD_COPY_BUFFER;
    copy.header.size = sizeof(copy);
    copy.src_arena_offset = fill.dest_arena_offset + fill.offset;
    copy.dst_arena_offset = copy.src_arena_offset + 256;
    copy.size = fill.size;
    memcpy(arena, &fill, sizeof(fill));
    memcpy(arena + sizeof(fill), &copy, sizeof(copy));
    CHECK(execute(sizeof(fill) + sizeof(copy)) == 0);
    for (unsigned i = 0; i < fill.size; ++i) {
        uint8_t expected = (uint8_t)(fill.value >> ((i & 3) * 8));
        CHECK(arena[copy.src_arena_offset + i] == expected);
        CHECK(arena[copy.dst_arena_offset + i] == expected);
    }
    CHECK(arena[copy.src_arena_offset - 1] == 0xa5 &&
          arena[copy.src_arena_offset + fill.size] == 0xa5);
    puts("vkd: fill/copy fence readback passed");
    /* Repeat submissions and exceed the old 64-element touched-range table. */
    for (unsigned i = 0; i < MX_VK_MAX_COMMANDS_PER_SUBMIT; ++i) {
        memcpy(arena + i * sizeof(fill), &fill, sizeof(fill));
    }
    CHECK(execute(MX_VK_MAX_COMMANDS_PER_SUBMIT * sizeof(fill)) == 0);
    memcpy(arena + MX_VK_MAX_COMMANDS_PER_SUBMIT * sizeof(fill), &fill, sizeof(fill));
    CHECK(execute((MX_VK_MAX_COMMANDS_PER_SUBMIT + 1) * sizeof(fill)) != 0);
    CHECK(vkd_vulkan_execute(NULL, sizeof(arena), 0, sizeof(fill)) != 0);
    CHECK(vkd_vulkan_execute(arena, MX_VK_SESSION_MAX_ARENA_BYTES + 1, 0, sizeof(fill)) != 0);
    CHECK(vkd_vulkan_execute(arena, sizeof(arena), UINT32_MAX, sizeof(fill)) != 0);
    CHECK(vkd_vulkan_execute(arena, sizeof(arena), 1, sizeof(fill)) != 0);
    CHECK(execute(0) != 0 && execute(sizeof(fill) - 1) != 0);
    for (unsigned i = 0; i < 12; ++i) {
        MxVkCmdFillBuffer invalid = fill;
        switch (i) {
        case 0:
            invalid.header.size = sizeof(MxVkCommandHeader);
            break;
        case 1:
            invalid.header.size = sizeof(fill) + MX_VK_COMMAND_ALIGNMENT;
            break;
        case 2:
            invalid.header.flags = 1;
            break;
        case 3:
            invalid.dest_arena_offset = UINT64_MAX;
            break;
        case 4:
            invalid.offset = UINT64_MAX;
            break;
        case 5:
            invalid.size = UINT64_MAX;
            break;
        case 6:
            invalid.size = 0;
            break;
        case 7:
            invalid.offset = 1;
            break;
        case 8:
            invalid.dest_arena_offset = 0;
            break;
        case 9:
            invalid.size = 3;
            break;
        case 10:
            invalid.reserved0 = 1;
            break;
        case 11:
            invalid.header.opcode = UINT32_MAX;
            break;
        }
        CHECK(rejected(&invalid, sizeof(invalid)));
    }
    copy.dst_arena_offset = copy.src_arena_offset + 1;
    CHECK(rejected(&copy, sizeof(copy)));
    copy.dst_arena_offset = sizeof(arena) - 1;
    CHECK(rejected(&copy, sizeof(copy)));
    copy.src_arena_offset = UINT64_MAX;
    CHECK(rejected(&copy, sizeof(copy)));
    /* Validation is transactional even when a valid write precedes the error. */
    memcpy(arena, &fill, sizeof(fill));
    memcpy(arena + sizeof(fill), &copy, sizeof(copy));
    memcpy(before, arena, sizeof(arena));
    CHECK(execute(sizeof(fill) + sizeof(copy)) != 0);
    CHECK(memcmp(before, arena, sizeof(arena)) == 0);
    memcpy(arena, &fill, sizeof(fill));
    CHECK(execute(sizeof(fill)) == 0);
    uint32_t copied = 123;
    CHECK(vkd_vulkan_present(arena, sizeof(arena), &copied) != 0 && copied == 0);
    vkd_vulkan_shutdown();
    CHECK(!vkd_vulkan_available());
    vkd_vulkan_shutdown();
    CHECK(vkd_vulkan_init() == 0);
    vkd_vulkan_shutdown();
    puts("vkd: bounds/reuse/cleanup passed");
    puts("vkd: acceptance passed");
    return vkd_runtime_exit(0);
fail:
    vkd_vulkan_shutdown();
    return vkd_runtime_exit(1);
}

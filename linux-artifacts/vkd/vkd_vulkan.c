/* Bounded Vulkan transfer executor. All resources are created once at startup;
 * each submit snapshots commands before validation and stages one arena buffer.
 * A single buffer preserves aliasing and fill -> copy dependencies across
 * commands. Native object ownership remains the session broker's responsibility.
 */
#include "vkd_vulkan.h"
#include "midix/abi/vulkan_commands.h"

#if defined(VKD_HAVE_VULKAN)
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

struct vkd_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    int ready;
    int pending;
    uint8_t commands[MX_VK_COMMAND_REGION_BYTES];
};
static struct vkd_device state;

int vkd_vulkan_available(void) {
    return state.ready;
}

void vkd_vulkan_shutdown(void) {
    /* Also handles every partially initialized state. Pending work must retire
     * before freeing device memory, including after a failed bounded wait. */
    if (state.device) {
        if (state.pending) {
            (void)vkDeviceWaitIdle(state.device);
        }
        if (state.mapped) {
            vkUnmapMemory(state.device, state.memory);
        }
        if (state.buffer) {
            vkDestroyBuffer(state.device, state.buffer, NULL);
        }
        if (state.memory) {
            vkFreeMemory(state.device, state.memory, NULL);
        }
        if (state.fence) {
            vkDestroyFence(state.device, state.fence, NULL);
        }
        if (state.pool) {
            vkDestroyCommandPool(state.device, state.pool, NULL);
        }
        vkDestroyDevice(state.device, NULL);
    }
    if (state.instance) {
        vkDestroyInstance(state.instance, NULL);
    }
    memset(&state, 0, sizeof(state));
}

int vkd_vulkan_init(void) {
    if (state.ready) {
        return 0;
    }
    vkd_vulkan_shutdown();
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "dango-vkd",
                             .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                     .pApplicationInfo = &app};
    if (vkCreateInstance(&instance, NULL, &state.instance) != VK_SUCCESS) {
        goto fail;
    }

    /* The rootfs selects its one lavapipe ICD explicitly. Refuse a non-CPU
     * device rather than binding physical hardware through this profile. */
    uint32_t count = 1;
    VkResult result = vkEnumeratePhysicalDevices(state.instance, &count, &state.physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count != 1) {
        goto fail;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(state.physical, &properties);
    if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
        goto fail;
    }
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(state.physical, &family_count, NULL);
    if (!family_count) {
        goto fail;
    }
    VkQueueFamilyProperties* families = calloc(family_count, sizeof(*families));
    if (!families) {
        goto fail;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(state.physical, &family_count, families);
    uint32_t family_index = family_count;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueCount &&
            (families[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT))) {
            family_index = i;
            break;
        }
    }
    free(families);
    if (family_index == family_count) {
        goto fail;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = family_index,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &queue};
    if (vkCreateDevice(state.physical, &device, NULL, &state.device) != VK_SUCCESS) {
        goto fail;
    }
    vkGetDeviceQueue(state.device, family_index, 0, &state.queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = family_index};
    if (vkCreateCommandPool(state.device, &pool, NULL, &state.pool) != VK_SUCCESS) {
        goto fail;
    }
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = state.pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(state.device, &command, &state.command) != VK_SUCCESS) {
        goto fail;
    }
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(state.device, &fence, NULL, &state.fence) != VK_SUCCESS) {
        goto fail;
    }
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = MX_VK_SESSION_MAX_ARENA_BYTES,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(state.device, &buffer, NULL, &state.buffer) != VK_SUCCESS) {
        goto fail;
    }
    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetBufferMemoryRequirements(state.device, state.buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(state.physical, &memory_properties);
    uint32_t memory_type = memory_properties.memoryTypeCount;
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == memory_properties.memoryTypeCount) {
        goto fail;
    }
    VkMemoryAllocateInfo memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = requirements.size,
                                   .memoryTypeIndex = memory_type};
    if (vkAllocateMemory(state.device, &memory, NULL, &state.memory) != VK_SUCCESS ||
        vkBindBufferMemory(state.device, state.buffer, state.memory, 0) != VK_SUCCESS ||
        vkMapMemory(state.device, state.memory, 0, VK_WHOLE_SIZE, 0, &state.mapped) != VK_SUCCESS) {
        goto fail;
    }
    state.ready = 1;
    return 0;
fail:
    vkd_vulkan_shutdown();
    return -1;
}

static int validate(uint32_t arena_bytes, uint32_t command_bytes) {
    uint32_t count = 0;
    for (uint32_t off = 0; off < command_bytes;) {
        MxVkCommand cmd;
        if (++count > MX_VK_MAX_COMMANDS_PER_SUBMIT ||
            mx_vk_command_decode(state.commands + off, command_bytes - off, &cmd)) {
            return -1;
        }
        switch (cmd.header.opcode) {
        case VK_CMD_FILL_BUFFER:
            if (cmd.fill.dest_arena_offset < MX_VK_COMMAND_REGION_BYTES ||
                cmd.fill.dest_arena_offset > arena_bytes ||
                cmd.fill.offset > arena_bytes - cmd.fill.dest_arena_offset ||
                !mx_vk_data_range_valid(cmd.fill.dest_arena_offset + cmd.fill.offset, cmd.fill.size,
                                        arena_bytes) ||
                ((cmd.fill.dest_arena_offset + cmd.fill.offset) & 3) || (cmd.fill.size & 3)) {
                return -1;
            }
            break;
        case VK_CMD_COPY_BUFFER:
            if (!mx_vk_data_range_valid(cmd.copy.src_arena_offset, cmd.copy.size, arena_bytes) ||
                !mx_vk_data_range_valid(cmd.copy.dst_arena_offset, cmd.copy.size, arena_bytes)) {
                return -1;
            }
            /* vkCmdCopyBuffer forbids overlapping source/destination regions. */
            if (cmd.copy.src_arena_offset < cmd.copy.dst_arena_offset + cmd.copy.size &&
                cmd.copy.dst_arena_offset < cmd.copy.src_arena_offset + cmd.copy.size) {
                return -1;
            }
            break;
        case VK_CMD_PIPELINE_BARRIER:
            break;
        default:
            return -1; /* No graphics/compute pipeline yet. */
        }
        off += cmd.header.size;
    }
    return 0;
}

static void barrier(VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = dst_access};
    vkCmdPipelineBarrier(state.command, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0, 1, &memory, 0,
                         NULL, 0, NULL);
}

int vkd_vulkan_execute(uint8_t* arena, uint32_t arena_bytes, uint32_t command_offset,
                       uint32_t command_bytes) {
    if (!state.ready || !arena || arena_bytes <= MX_VK_COMMAND_REGION_BYTES ||
        arena_bytes > MX_VK_SESSION_MAX_ARENA_BYTES || !command_bytes ||
        command_offset % MX_VK_COMMAND_ALIGNMENT || command_bytes % MX_VK_COMMAND_ALIGNMENT ||
        command_offset >= MX_VK_COMMAND_REGION_BYTES ||
        command_bytes > MX_VK_COMMAND_REGION_BYTES - command_offset) {
        return -1;
    }
    memcpy(state.commands, arena + command_offset, command_bytes);
    if (validate(arena_bytes, command_bytes)) {
        return -1;
    }
    if (vkResetCommandBuffer(state.command, 0) != VK_SUCCESS ||
        vkResetFences(state.device, 1, &state.fence) != VK_SUCCESS) {
        goto fail;
    }
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(state.command, &begin) != VK_SUCCESS) {
        goto fail;
    }
    memcpy((uint8_t*)state.mapped + MX_VK_COMMAND_REGION_BYTES, arena + MX_VK_COMMAND_REGION_BYTES,
           arena_bytes - MX_VK_COMMAND_REGION_BYTES);
    for (uint32_t off = 0; off < command_bytes;) {
        MxVkCommand cmd;
        (void)mx_vk_command_decode(state.commands + off, command_bytes - off, &cmd);
        if (cmd.header.opcode == VK_CMD_FILL_BUFFER) {
            vkCmdFillBuffer(state.command, state.buffer,
                            cmd.fill.dest_arena_offset + cmd.fill.offset, cmd.fill.size,
                            cmd.fill.value);
        } else if (cmd.header.opcode == VK_CMD_COPY_BUFFER) {
            VkBufferCopy copy = {.srcOffset = cmd.copy.src_arena_offset,
                                 .dstOffset = cmd.copy.dst_arena_offset,
                                 .size = cmd.copy.size};
            vkCmdCopyBuffer(state.command, state.buffer, state.buffer, 1, &copy);
        }
        /* Preserve record order, including RAW/WAW aliases and explicit barriers. */
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        off += cmd.header.size;
    }
    barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if (vkEndCommandBuffer(state.command) != VK_SUCCESS) {
        goto fail;
    }
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &state.command};
    if (vkQueueSubmit(state.queue, 1, &submit, state.fence) != VK_SUCCESS) {
        goto fail;
    }
    state.pending = 1;
    if (vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, VKD_SUBMIT_TIMEOUT_NS) !=
        VK_SUCCESS) {
        goto fail;
    }
    state.pending = 0;
    memcpy(arena + MX_VK_COMMAND_REGION_BYTES, (uint8_t*)state.mapped + MX_VK_COMMAND_REGION_BYTES,
           arena_bytes - MX_VK_COMMAND_REGION_BYTES);
    return 0;
fail:
    state.ready = 0; /* Never reuse resources whose completion is uncertain. */
    return -1;
}
#else
int vkd_vulkan_available(void) {
    return 0;
}
int vkd_vulkan_init(void) {
    return -1;
}
void vkd_vulkan_shutdown(void) {}
int vkd_vulkan_execute(uint8_t* arena, uint32_t arena_bytes, uint32_t command_offset,
                       uint32_t command_bytes) {
    (void)arena;
    (void)arena_bytes;
    (void)command_offset;
    (void)command_bytes;
    return -1;
}
#endif

/* No WSI backend is implemented here. Never report bytes copied without doing
 * a copy or advertise WSI merely because device creation succeeded. */
int vkd_vulkan_present(const uint8_t* image, uint32_t image_bytes, uint32_t* copied_out) {
    (void)image;
    (void)image_bytes;
    if (copied_out) {
        *copied_out = 0;
    }
    return -1;
}

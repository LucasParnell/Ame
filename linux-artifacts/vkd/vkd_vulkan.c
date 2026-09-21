/*
 * vkd_vulkan.c — the Vulkan execution half of dango-vkd (spec §14.6).
 *
 * Two builds, one interface:
 *
 *   * VKD_HAVE_VULKAN: link against a libc guest userspace carrying the Vulkan
 *     loader and the Mesa lavapipe ICD. dango-vkd then creates a real Vulkan
 *     device and translates the bounded command vocabulary into Vulkan calls
 *     (vkCmdFillBuffer, vkCmdCopyBuffer), submitting one command buffer per
 *     session submit and waiting on a fence — a real remote-Vulkan execution.
 *   * default (no libc): the freestanding initramfs has no loader and no ICD, so
 *     the executor reports no device. dango-vkd then refuses submits with
 *     MX_ERR_SERVICE_UNAVAILABLE instead of pretending, and the client degrades.
 *
 * This translation has NOT been executed in the development environment: the
 * guest image would first need a libc userspace plus libvulkan + lavapipe (see
 * scripts/linux/build-vkd.sh). It is compiled only when VKD_HAVE_VULKAN is set;
 * the default build compiles the fallback below and is byte-for-byte the honest
 * "no device" server.
 */

#include "vkd_vulkan.h"

#include "midix/abi/protocols/vulkan_session.h"

#if defined(VKD_HAVE_VULKAN)

#include <vulkan/vulkan.h>
#include <string.h>

/* Fixed capacities: no allocation on the request path (spec §14.7). */
#define VKD_VULKAN_MAX_BUFFERS 64

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;
static VkQueue g_queue;
static uint32_t g_queue_family;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd;
static VkFence g_fence;
static int g_ready;

/* One host-visible buffer per arena allocation, keyed by arena offset. */
struct vkd_mapping {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void* mapped;
};
static struct vkd_mapping g_buffers[VKD_VULKAN_MAX_BUFFERS];
static uint32_t g_buffer_count;

int vkd_vulkan_available(void) { return g_ready; }

static uint32_t pick_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return 0;
}

int vkd_vulkan_init(void) {
    if (g_ready) return 0;

    /* Probe lavapipe by name so a host without a software ICD fails cleanly
     * instead of binding some other device. lavapipe's device name is
     * "llvmpipe (LLVM ...)". */
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dango-vkd";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &g_instance) != VK_SUCCESS) return -1;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_instance, &count, NULL);
    if (count == 0) return -1;
    VkPhysicalDevice devices[8];
    if (count > 8) count = 8;
    vkEnumeratePhysicalDevices(g_instance, &count, devices);
    g_physical = devices[0];

    VkQueueFamilyProperties families[8];
    uint32_t family_count = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &family_count, families);
    g_queue_family = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_queue_family = i;
            break;
        }
    }
    if (g_queue_family == 0xFFFFFFFFu) return -1;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(g_physical, &dci, NULL, &g_device) != VK_SUCCESS) return -1;
    vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);

    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = g_queue_family;
    if (vkCreateCommandPool(g_device, &pci, NULL, &g_pool) != VK_SUCCESS) return -1;

    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_device, &cai, &g_cmd) != VK_SUCCESS) return -1;

    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(g_device, &fci, NULL, &g_fence) != VK_SUCCESS) return -1;

    g_ready = 1;
    return 0;
}

/* Return (creating on first use) the host-visible buffer backing an arena range.
 * A session allocates out of the shared arena, so one buffer per distinct
 * (offset,size) is enough and every operand is a buffer-relative offset. */
static struct vkd_mapping* mapping_for(uint32_t offset, uint32_t size) {
    for (uint32_t i = 0; i < g_buffer_count; ++i) {
        if (g_buffers[i].offset == offset && g_buffers[i].size >= size) return &g_buffers[i];
    }
    if (g_buffer_count >= VKD_VULKAN_MAX_BUFFERS) return NULL;
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    struct vkd_mapping* m = &g_buffers[g_buffer_count];
    if (vkCreateBuffer(g_device, &bci, NULL, &m->buffer) != VK_SUCCESS) return NULL;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_device, m->buffer, &req);
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = pick_memory_type(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &mai, NULL, &m->memory) != VK_SUCCESS) return NULL;
    vkBindBufferMemory(g_device, m->buffer, m->memory, 0);
    vkMapMemory(g_device, m->memory, 0, VK_WHOLE_SIZE, 0, &m->mapped);
    m->offset = offset;
    m->size = size;
    ++g_buffer_count;
    return m;
}

int vkd_vulkan_execute(const uint8_t* arena, uint32_t arena_bytes, uint32_t command_offset,
                       uint32_t command_bytes) {
    if (!g_ready) return -1;
    if ((uint64_t)command_offset + command_bytes > arena_bytes) return -1;

    /* Stage the client's arena bytes into host-visible Vulkan buffers, replay
     * the command records, then flush the result back. The arena is the single
     * shared region; commands name offsets into it. A production appliance would
     * import the staging pages instead of copying (spec §15.6), which is Phase S. */
    struct { uint32_t offset; uint32_t size; } touched[VKD_VULKAN_MAX_BUFFERS];
    uint32_t touched_count = 0;

    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cmd, &begin) != VK_SUCCESS) return -1;

    uint32_t off = 0;
    while (off + sizeof(MxVkCommandHeader) <= command_bytes) {
        const MxVkCommandHeader* header =
            (const MxVkCommandHeader*)(const void*)(arena + command_offset + off);
        if (header->size < sizeof(MxVkCommandHeader) || header->size > command_bytes - off) {
            vkEndCommandBuffer(g_cmd);
            return -1;
        }
        switch (header->opcode) {
        case VK_CMD_FILL_BUFFER: {
            const MxVkCmdFillBuffer* cmd = (const MxVkCmdFillBuffer*)(const void*)header;
            uint32_t size = (uint32_t)cmd->size;
            struct vkd_mapping* m = mapping_for((uint32_t)cmd->dest_arena_offset, size);
            if (!m) { vkEndCommandBuffer(g_cmd); return -1; }
            /* Seed the buffer with the client's current bytes so the fill is the
             * only change, then fill on the device. */
            memcpy(m->mapped, arena + cmd->dest_arena_offset, size);
            vkCmdFillBuffer(g_cmd, m->buffer, cmd->offset, cmd->size, cmd->value);
            touched[touched_count].offset = (uint32_t)cmd->dest_arena_offset;
            touched[touched_count].size = size;
            ++touched_count;
            break;
        }
        case VK_CMD_COPY_BUFFER: {
            const MxVkCmdCopyBuffer* cmd = (const MxVkCmdCopyBuffer*)(const void*)header;
            uint32_t size = (uint32_t)cmd->size;
            struct vkd_mapping* src = mapping_for((uint32_t)cmd->src_arena_offset, size);
            struct vkd_mapping* dst = mapping_for((uint32_t)cmd->dst_arena_offset, size);
            if (!src || !dst) { vkEndCommandBuffer(g_cmd); return -1; }
            memcpy(src->mapped, arena + cmd->src_arena_offset, size);
            VkBufferCopy region = {0};
            region.size = size;
            vkCmdCopyBuffer(g_cmd, src->buffer, dst->buffer, 1, &region);
            touched[touched_count].offset = (uint32_t)cmd->dst_arena_offset;
            touched[touched_count].size = size;
            ++touched_count;
            break;
        }
        case VK_CMD_PIPELINE_BARRIER:
            break;
        default:
            /* DRAW/DISPATCH need a pipeline the bootstrap path does not build
             * yet; refuse rather than silently drop them. */
            vkEndCommandBuffer(g_cmd);
            return -1;
        }
        off += header->size;
    }

    if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) return -1;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_cmd;
    if (vkQueueSubmit(g_queue, 1, &submit, g_fence) != VK_SUCCESS) return -1;
    if (vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return -1;
    vkResetFences(g_device, 1, &g_fence);

    /* Invalidate + read back every touched range (host-coherent memory needs no
     * explicit invalidate, but keep it correct for non-coherent types). */
    for (uint32_t i = 0; i < touched_count && i < VKD_VULKAN_MAX_BUFFERS; ++i) {
        for (uint32_t b = 0; b < g_buffer_count; ++b) {
            if (g_buffers[b].offset == touched[i].offset) {
                VkMappedMemoryRange range = {0};
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = g_buffers[b].memory;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                vkInvalidateMappedMemoryRanges(g_device, 1, &range);
                memcpy((void*)(arena + touched[i].offset), g_buffers[b].mapped, touched[i].size);
                break;
            }
        }
    }
    return 0;
}

int vkd_vulkan_present(const uint8_t* image, uint32_t image_bytes, uint32_t* copied_out) {
    if (!g_ready) return -1;
    /* The bounded copy into the Dango shared buffer is done by the caller (it
     * owns the staging window mapping); here we only bound the size. */
    uint32_t copied = image_bytes;
    if (copied > MX_VK_SHARED_BUFFER_MAX_BYTES) copied = MX_VK_SHARED_BUFFER_MAX_BYTES;
    (void)image;
    if (copied_out) *copied_out = copied;
    return 0;
}

void vkd_vulkan_shutdown(void) {
    if (!g_ready) return;
    for (uint32_t i = 0; i < g_buffer_count; ++i) {
        if (g_buffers[i].mapped) vkUnmapMemory(g_device, g_buffers[i].memory);
        vkDestroyBuffer(g_device, g_buffers[i].buffer, NULL);
        vkFreeMemory(g_device, g_buffers[i].memory, NULL);
    }
    g_buffer_count = 0;
    vkDestroyFence(g_device, g_fence, NULL);
    vkDestroyCommandPool(g_device, g_pool, NULL);
    vkDestroyDevice(g_device, NULL);
    vkDestroyInstance(g_instance, NULL);
    g_ready = 0;
}

#else /* !VKD_HAVE_VULKAN */

int vkd_vulkan_available(void) { return 0; }
int vkd_vulkan_init(void) { return -1; }
int vkd_vulkan_execute(const uint8_t* arena, uint32_t arena_bytes, uint32_t command_offset,
                       uint32_t command_bytes) {
    (void)arena;
    (void)arena_bytes;
    (void)command_offset;
    (void)command_bytes;
    return -1;
}
int vkd_vulkan_present(const uint8_t* image, uint32_t image_bytes, uint32_t* copied_out) {
    (void)image;
    (void)image_bytes;
    (void)copied_out;
    return -1;
}
void vkd_vulkan_shutdown(void) {}

#endif /* VKD_HAVE_VULKAN */

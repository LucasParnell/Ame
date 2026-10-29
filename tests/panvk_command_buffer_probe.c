/* SPDX-License-Identifier: MIT */
/* Public-API lifecycle and allocation-failure probe; no GPU submission. */
#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct allocation { void *base; size_t size; };
struct allocator { unsigned live; int fail_next; };
static void *VKAPI_PTR allocate(void *user, size_t size, size_t alignment,
                               VkSystemAllocationScope scope)
{
    struct allocator *a = user;
    (void)scope;
    if (a->fail_next) { a->fail_next = 0; return NULL; }
    if (alignment < _Alignof(struct allocation)) alignment = _Alignof(struct allocation);
    void *base = malloc(size + alignment - 1 + sizeof(struct allocation));
    if (!base) return NULL;
    uintptr_t address = ((uintptr_t)base + sizeof(struct allocation) + alignment - 1)
                        & ~(uintptr_t)(alignment - 1);
    struct allocation *header = (struct allocation *)address - 1;
    *header = (struct allocation){base, size};
    ++a->live;
    return (void *)address;
}
static void VKAPI_PTR release(void *user, void *ptr)
{
    if (!ptr) return;
    struct allocator *a = user;
    free(((struct allocation *)ptr - 1)->base);
    --a->live;
}
static void *VKAPI_PTR resize(void *user, void *ptr, size_t size, size_t alignment,
                             VkSystemAllocationScope scope)
{
    if (!size) { release(user, ptr); return NULL; }
    void *next = allocate(user, size, alignment, scope);
    if (next && ptr) {
        size_t old = ((struct allocation *)ptr - 1)->size;
        memcpy(next, ptr, old < size ? old : size);
        release(user, ptr);
    }
    return next;
}
#define FUNCTIONS(X) \
 X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(DestroyDevice) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(ResetCommandPool) \
 X(CreateEvent) X(DestroyEvent) X(CmdSetEvent) X(CmdResetEvent) X(CmdWaitEvents)
#define DECLARE(name) PFN_vk##name name;
FUNCTIONS(DECLARE)
#undef DECLARE
#define CHECK(expr) do { VkResult r = (expr); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s returned %d at %d\n", #expr, r, __LINE__); return 1; } } while (0)
int main(void)
{
    void *library = dlopen("libvulkan_panfrost.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
    if (!get) return 1;
    CreateInstance = (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
    VkInstance instance;
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    CHECK(CreateInstance(&ii, NULL, &instance));
#define LOAD(name) name = (PFN_vk##name)get(instance, "vk" #name); if (!name) return 1;
    FUNCTIONS(LOAD)
#undef LOAD
    uint32_t count = 1;
    VkPhysicalDevice physical;
    CHECK(EnumeratePhysicalDevices(instance, &count, &physical));
    if (count != 1) return 1;
    uint32_t families = 0;
    GetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    if (!families) return 1;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    struct allocator state = {0};
    VkAllocationCallbacks callbacks = {.pUserData = &state, .pfnAllocation = allocate,
        .pfnReallocation = resize, .pfnFree = release};
    VkDevice device;
    CHECK(CreateDevice(physical, &di, &callbacks, &device));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0};
    VkCommandPool pool;
    CHECK(CreateCommandPool(device, &pi, NULL, &pool));
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pInheritanceInfo = &inheritance};
    VkEventCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent event;
    CHECK(CreateEvent(device, &ei, NULL, &event));
    for (unsigned level = 0; level < 2; ++level) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool, .level = (VkCommandBufferLevel)level, .commandBufferCount = 1};
        VkCommandBuffer command;
        CHECK(AllocateCommandBuffers(device, &ai, &command));
        state.fail_next = 1;
        VkResult result = BeginCommandBuffer(command, &begin);
        if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.fail_next) {
            fprintf(stderr, "BeginCommandBuffer must report injected host OOM: %d\n", result);
            return 1;
        }
        CHECK(ResetCommandBuffer(command, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
        CHECK(BeginCommandBuffer(command, &begin));
        CHECK(EndCommandBuffer(command));
        CHECK(ResetCommandBuffer(command, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT));
        state.fail_next = 1;
        result = BeginCommandBuffer(command, &begin);
        if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.fail_next) {
            fprintf(stderr, "Resource-release reset retained recording allocation: %d\n", result);
            return 1;
        }
        CHECK(ResetCommandBuffer(command, 0));
        for (unsigned round = 0; round < 20; ++round) {
            CHECK(BeginCommandBuffer(command, &begin));
            CHECK(EndCommandBuffer(command));
            /* Begin without explicit reset also exercises implicit reuse. */
            CHECK(BeginCommandBuffer(command, &begin));
            CHECK(EndCommandBuffer(command));
            CHECK(ResetCommandPool(device, pool, round & 1 ? VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT : 0));
        }
        /* Secondary recording is deferred by the common runtime. Its batch
         * allocation happens when replayed into a primary command buffer. */
        if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
            CHECK(BeginCommandBuffer(command, &begin));
            state.fail_next = 1;
            CmdSetEvent(command, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            CmdResetEvent(command, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            CmdWaitEvents(command, 1, &event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0, NULL);
            result = EndCommandBuffer(command);
            if (result != VK_ERROR_OUT_OF_HOST_MEMORY || state.fail_next) {
                fprintf(stderr, "EndCommandBuffer must retain batch allocation OOM: %d\n", result);
                return 1;
            }
            CHECK(ResetCommandBuffer(command, 0));
        }
    }
    DestroyEvent(device, event, NULL);
    DestroyCommandPool(device, pool, NULL);
    DestroyDevice(device, &callbacks);
    DestroyInstance(instance, NULL);
    dlclose(library);
    if (state.live) { fprintf(stderr, "live device allocations: %u\n", state.live); return 1; }
    puts("PanVK primary/secondary allocation failure, reset and reuse passed; no GPU submission.");
    return 0;
}

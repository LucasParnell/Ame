#include <vulkan/vulkan.h>
#include "ame/graphics.h"
#include "midix/vulkan/codec.h"
#include "vk-rpc-types.h"
#include "../linux-artifacts/panfrost-renderer/vulkan_rpc.h"
#include "../linux-artifacts/panvk-renderer/renderer.h"
#include "../linux-artifacts/panvk-renderer/suite.h"
#include "midix/vulkan/present.h"
#include <cassert>
#include <cstdio>
#include <cstring>
extern "C" void ame_native_vk_set_logger(void (*logger)(const char*,unsigned));
namespace ame::graphics {
static ame_gfx_reply last;
const ame_gfx_reply& completion(){return last;}
bool exchange(void* p,mx_u32_t bytes,mx_u32_t api,mx_u32_t flags,ame_gfx_reply& reply) {
    assert(api==AME_GFX_API_VULKAN);
    alignas(16) unsigned char packet[AME_VK_WIRE_BYTES];
    std::memcpy(packet,p,bytes);
    if(reinterpret_cast<ame::vk::Header*>(packet)->command==ame::vk::Present) {
        static uint32_t pixels[512*512];int finish=0;
        assert(flags&AME_GFX_PRESENT);
        reply.status=ame_vulkan_rpc_present(packet,bytes,pixels,&reply.checksum,&finish);
    } else {assert(flags==0);reply.status=ame_vulkan_rpc(packet,bytes);}
    last=reply;
    if(reply.status)return false;
    std::memcpy(p,packet,bytes);return true;
}
}
static void codec_checks() {
    using namespace ame::vk;
    Encoder encoder;
    VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="nested input"};
    VkInstanceCreateInfo create{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance output{};
    Args_vkCreateInstance args{&create,nullptr,&output};
    uint32_t root=encoder.object(T_Args_vkCreateInstance,&args,1,true,false);assert(encoder.good);
    auto* header=reinterpret_cast<Header*>(encoder.data);
    *header={AME_VK_WIRE_VERSION,C_vkCreateInstance,encoder.used,root,0,0};
    Handles handles;
    Decoder decoded{encoder.data,encoder.used,handles};
    assert(decoded.region(root,sizeof(args)));decoded.object(T_Args_vkCreateInstance,root,1,true,false);assert(decoded.good);
    auto& actual=*reinterpret_cast<Args_vkCreateInstance*>(encoder.data+root);
    assert(!std::strcmp(actual.pCreateInfo->pApplicationInfo->pApplicationName,"nested input"));
    *actual.pInstance=reinterpret_cast<VkInstance>(0x12345000);
    decoded.finish();encoder.copy_back();assert(output && reinterpret_cast<uint64_t>(output)!=0x12345000);
    uint64_t token=reinterpret_cast<uint64_t>(output);
    bool good=true;assert(handles.actual(T_VkInstance,token,good)==0x12345000 && good);
    handles.erase(T_VkInstance,0x12345000);handles.commit();
    handles.actual(T_VkInstance,token,good);assert(!good);
    // Every pointer allocation is disjoint; output aliases cannot forge a
    // different kind of driver object by overwriting an input handle.
    auto* packet_args=reinterpret_cast<Args_vkCreateInstance*>(encoder.data+root);
    packet_args->pInstance=reinterpret_cast<VkInstance*>(root);
    Decoder aliased{encoder.data,encoder.used,handles};
    assert(aliased.region(root,sizeof(args)));aliased.object(T_Args_vkCreateInstance,root,1,true,false);assert(!aliased.good);
    create.pNext=&app;
    Encoder unsupported;unsupported.object(T_Args_vkCreateInstance,&args,1,true,false);assert(!unsupported.good);
    std::puts("native Vulkan frontend: nested codec, handle retirement and malformed packet checks passed");
}
int main(int argc,char** argv) {
    codec_checks();
    if(argc>1 && !std::strcmp(argv[1],"--codec-only"))return 0;
    VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Ame native Vulkan",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance{};
    assert(vkCreateInstance(&ici,nullptr,&instance)==VK_SUCCESS && instance);
    uint32_t count=0;
    assert(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS && count);
    VkPhysicalDevice physical{};count=1;
    assert(vkEnumeratePhysicalDevices(instance,&count,&physical)==VK_SUCCESS && physical);
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    std::printf("native Vulkan frontend: device=%s\n",props.deviceName);
    count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);assert(count);
    VkQueueFamilyProperties queues[32]{};assert(count<=32);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues);
    unsigned family=0;while(family<count && !(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;
    assert(family<count);
    float priority=1;
    VkDeviceQueueCreateInfo qi{.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VkDevice device{};assert(vkCreateDevice(physical,&di,nullptr,&device)==VK_SUCCESS);
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);assert(queue);
    VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4096,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer{};assert(vkCreateBuffer(device,&bi,nullptr,&buffer)==VK_SUCCESS);
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);assert(req.memoryTypeBits);
    VkMemoryAllocateInfo ai{.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=static_cast<uint32_t>(__builtin_ctz(req.memoryTypeBits))};
    VkDeviceMemory mem{};assert(vkAllocateMemory(device,&ai,nullptr,&mem)==VK_SUCCESS);
    assert(vkBindBufferMemory(device,buffer,mem,0)==VK_SUCCESS);
    VkCommandPoolCreateInfo ci{.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool{};assert(vkCreateCommandPool(device,&ci,nullptr,&pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo ca{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command{};assert(vkAllocateCommandBuffers(device,&ca,&command)==VK_SUCCESS);
    VkCommandBufferBeginInfo cb{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(command,&cb)==VK_SUCCESS);
    vkCmdFillBuffer(command,buffer,0,4096,0x12345678);
    assert(vkEndCommandBuffer(command)==VK_SUCCESS);
    VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    assert(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);
    assert(vkQueueWaitIdle(queue)==VK_SUCCESS);
    void* mapped=nullptr;
    assert(vkMapMemory(device,mem,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
    VkMappedMemoryRange range{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=mem,.size=VK_WHOLE_SIZE};
    assert(vkInvalidateMappedMemoryRanges(device,1,&range)==VK_SUCCESS);
    for(unsigned i=0;i<1024;i++)assert(static_cast<uint32_t*>(mapped)[i]==0x12345678);
    for(unsigned i=0;i<1024;i++)static_cast<uint32_t*>(mapped)[i]=0xabcd0000+i;
    assert(vkFlushMappedMemoryRanges(device,1,&range)==VK_SUCCESS);
    std::memset(mapped,0,4096);
    assert(vkInvalidateMappedMemoryRanges(device,1,&range)==VK_SUCCESS);
    for(unsigned i=0;i<1024;i++)assert(static_cast<uint32_t*>(mapped)[i]==0xabcd0000+i);
    vkUnmapMemory(device,mem);
    vkDestroyCommandPool(device,pool,nullptr);
    vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,mem,nullptr);
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
#ifdef AME_NATIVE_VULKAN
    std::setbuf(stdout,nullptr);
    ame_native_vk_set_logger([](const char* p,unsigned size){std::fwrite(p,1,size,stdout);});
    assert(panvk_suite_run(nullptr,0)==0);
    auto* triangle=panvk_triangle_create(nullptr,0);assert(triangle);
    static uint32_t pixels[512*512];uint32_t checksum=0;
    assert(panvk_triangle_frame(triangle,0,pixels,&checksum)==0);
    assert(panvk_triangle_verify_pixels(pixels));
    assert(panvk_triangle_verify_load(triangle,pixels,&checksum)==0);
    assert(panvk_triangle_present_frame(triangle,0,&checksum)==0);
    assert(panvk_triangle_present_finish()==0);
    panvk_triangle_destroy(triangle);
    std::puts("native Vulkan frontend: complete pixel suite, triangle and presentation passed");
#endif
    // Stale handles and out-of-range offsets fail without reaching the driver.
    ame::vk::Encoder encoded;
    ame::vk::Args_vkDeviceWaitIdle args{device};
    auto root=encoded.object(ame::vk::T_Args_vkDeviceWaitIdle,&args,1,true,false);
    auto* h=reinterpret_cast<ame::vk::Header*>(encoded.data);
    *h={AME_VK_WIRE_VERSION,ame::vk::C_vkDeviceWaitIdle,encoded.used,root,0,0};
    assert(ame_vulkan_rpc(encoded.data,encoded.used)==0 && h->result<0);
    h->root=encoded.used+8;
    assert(ame_vulkan_rpc(encoded.data,encoded.used)==0 && h->result<0);
    std::puts("native Vulkan frontend: create/record/submit/retire and malformed RPC checks passed");
}

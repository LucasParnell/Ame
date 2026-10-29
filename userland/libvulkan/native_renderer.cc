#include "ame/vulkan.h"
#include "ame/graphics.h"
#include "libk/printf.h"
#include <cstdarg>
#include <cstddef>
extern "C" {
alignas(16) static unsigned char triangle_storage[4096];
static bool triangle_live;
alignas(16) static unsigned char before_pixels[512*512*4];
void* ame_native_vk_calloc(size_t count,size_t size) {
    if(triangle_live || !size || count>sizeof(triangle_storage)/size)return nullptr;
    triangle_live=true;
    for(auto& byte:triangle_storage)byte=0;
    return triangle_storage;
}
void* ame_native_vk_malloc(size_t size) {return size<=sizeof(before_pixels) ? before_pixels : nullptr;}
void ame_native_vk_free(void* pointer) {if(pointer==triangle_storage)triangle_live=false;}
const char* ame_native_vk_strstr(const char* text,const char* needle) {
    for(;*text;text++) {const char *a=text,*b=needle;while(*a&&*a==*b){a++;b++;}if(!*b)return text;}
    return *needle ? nullptr : text;
}
static void (*diagnostic_log)(const char*,unsigned);
void ame_native_vk_set_logger(void (*logger)(const char*,unsigned)) {diagnostic_log=logger;}
int ame_native_vk_log(const char* format,...) {
    if(!diagnostic_log)return 0;
    char text[320];va_list args;va_start(args,format);
    int n=libk::vsnprintf(text,sizeof(text),format,args);va_end(args);
    if(n>0)diagnostic_log(text,static_cast<unsigned>(n)<sizeof(text) ? n : sizeof(text)-1);
    return n;
}
PFN_vkVoidFunction ame_native_vk_proc(VkInstance instance,const char* name) {return vkGetInstanceProcAddr(instance,name);}
VkResult ame_native_vk_present(VkDevice device,VkQueue queue,VkImage image,VkBool32 finish,uint32_t* checksum) {
    VkResult result=ameVkPresentDiagnosticImage(device,queue,image,finish);
    if(checksum)*checksum=ame::graphics::completion().checksum;
    return result;
}
}

#pragma once
#include <stdint.h>
#include <stddef.h>
/* Freestanding services supply storage and logging to the shared Vulkan client.
 * This file is private to that C translation unit, never a libc replacement. */
void* ame_native_vk_calloc(size_t count,size_t size);
void* ame_native_vk_malloc(size_t size);
void ame_native_vk_free(void* pointer);
int ame_native_vk_log(const char* format,...);
const char* ame_native_vk_strstr(const char* text,const char* needle);
void* memcpy(void* dest,const void* source,size_t size);
PFN_vkVoidFunction ame_native_vk_proc(VkInstance instance,const char* name);
VkResult ame_native_vk_present(VkDevice device,VkQueue queue,VkImage image,VkBool32 finish,uint32_t* checksum);
#define calloc ame_native_vk_calloc
#define malloc ame_native_vk_malloc
#define free ame_native_vk_free
#define fprintf(unused,...) ame_native_vk_log(__VA_ARGS__)
#define strstr ame_native_vk_strstr
#define isfinite __builtin_isfinite

void* memset(void* dest,int value,size_t size);
#define printf(...) ame_native_vk_log(__VA_ARGS__)
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define dlopen(name,flags) ((void)(name),(void)(flags),(void*)1)
#define dlsym(library,name) ((void)(library),(void)(name),(void*)ame_native_vk_proc)
#define dlclose(library) ((void)(library))
#define dlerror() "native Vulkan frontend"
#define getenv(name) ((void)(name),(char*)0)
#define setenv(name,value,replace) ((void)(name),(void)(value),(void)(replace),0)
#define fminf(a,b) ((a)<(b)?(a):(b))
#define fmaxf(a,b) ((a)>(b)?(a):(b))
#define fabsf __builtin_fabsf

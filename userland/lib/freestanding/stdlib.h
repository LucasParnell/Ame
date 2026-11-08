#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void free(void*);
void* aligned_alloc(size_t, size_t);
#ifdef __cplusplus
[[noreturn]] void abort();
[[noreturn]] void exit(int);
[[noreturn]] void _Exit(int);
}
#endif

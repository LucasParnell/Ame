#pragma once
#include <cstddef>
#include "midix/abi/types.h"
extern "C" {
void* malloc(size_t);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
void free(void*);
void* aligned_alloc(size_t alignment, size_t bytes);
}
namespace mochi::memory {
struct Statistics { mx_u64_t live, committed, limit, allocations, frees, growths; };
Statistics statistics();
}

#pragma once
#include <cstddef>
#include <cstdint>

namespace libk {

void*  memset(void* dst, int c, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
size_t strlen(const char* s);

// Big-endian read helpers — DTB is big-endian.
uint32_t be32(const void* p);
uint64_t be64(const void* p);

} // namespace libk

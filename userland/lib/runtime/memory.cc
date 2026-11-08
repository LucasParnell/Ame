#include "mochi/heap.h"
#include "mochi/managed_arena.h"
#include "mochi/memory.h"
#include "libk/spin.h"
#include <new>
namespace {
mochi::memory::Heap heap;
MxArenaCreate contract{};
mx_handle_t arena;
mx_u32_t mutex;
mx_u64_t allocations, frees, growths;
struct Guard {
    Guard() { libk::spin_lock(mutex); }
    ~Guard() { libk::spin_unlock(mutex); }
};
mx_u64_t grow(void*, mx_u64_t bytes) {
    if (mx_arena_resize(arena, bytes) != MX_OK) return 0;
    ++growths; return bytes;
}
bool initialize() {
    if (heap.initialized()) return true;
    if (mx_process_arena(&contract, &arena) != MX_OK) return false;
    if (heap.initialize(reinterpret_cast<void*>(contract.cage_base), contract.size,
                         contract.cage_size, nullptr, grow)) return true;
    (void)sys_cap_revoke(arena); arena = 0; return false;
}
void* aligned(mx_u64_t alignment, mx_u64_t size) {
    Guard guard;
    if (!initialize()) return nullptr;
    ++allocations; return heap.allocate_aligned(alignment, size);
}
}
extern "C" void* malloc(size_t bytes) {
    Guard guard;
    if (!initialize()) return nullptr;
    ++allocations; return heap.allocate(bytes);
}
extern "C" void free(void* pointer) {
    if (!pointer) return;
    Guard guard;
    if (!heap.release(pointer)) mx_exit(MX_ERR_FAULT);
    ++frees;
}
extern "C" void* realloc(void* pointer, size_t bytes) {
    Guard guard;
    if (!initialize()) return nullptr;
    ++allocations; return heap.reallocate(pointer, bytes);
}
extern "C" void* calloc(size_t count, size_t bytes) {
    if (count && bytes > ~size_t{0} / count) return nullptr;
    auto* result = malloc(count * bytes);
    if (result) libk::zero(result, count * bytes);
    return result;
}
extern "C" void* aligned_alloc(size_t alignment, size_t bytes) {
    if (!alignment || (alignment & (alignment - 1)) || bytes % alignment) return nullptr;
    return aligned(alignment, bytes);
}
extern "C" [[noreturn]] void _Exit(int status) { mx_exit(status); for (;;) sys_yield(); }
extern "C" [[noreturn]] void exit(int status) { _Exit(status); }
extern "C" [[noreturn]] void abort() { _Exit(MX_ERR_FAULT); }
void* operator new(size_t bytes) { auto* p = malloc(bytes); if (!p) _Exit(MX_ERR_NOMEM); return p; }
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void* operator new(size_t bytes, const std::nothrow_t&) noexcept { return malloc(bytes); }
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept { return malloc(bytes); }
void* operator new(size_t bytes, std::align_val_t alignment) {
    auto* p = aligned(static_cast<size_t>(alignment), bytes);
    if (!p) _Exit(MX_ERR_NOMEM); return p;
}
void* operator new[](size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { free(p); }
namespace mochi::memory {
Statistics statistics() {
    Guard guard;
    return {heap.live_bytes(), heap.committed(), contract.cage_size, allocations, frees, growths};
}
}

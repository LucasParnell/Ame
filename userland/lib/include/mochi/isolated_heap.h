#pragma once
#include "mochi/heap.h"
#include "mochi/managed_arena.h"
#include "libk/spin.h"
#include <new>

namespace mochi::memory {
// Ame's isoheap: one compile-time type owns a kernel-enforced virtual cage.
// The cage is sequestered until process exit, including after physical trim.
// Place instances in long-lived service state. No per-object syscall is needed.
template <typename T> class IsolatedHeap {
    Heap heap_;
    mx_handle_t arena_ = 0;
    mx_u32_t lock_ = 0;
    struct Guard {
        mx_u32_t& lock;
        explicit Guard(mx_u32_t& value) : lock(value) { libk::spin_lock(lock); }
        ~Guard() { libk::spin_unlock(lock); }
    };
    static mx_u64_t grow(void* context, mx_u64_t bytes) {
        auto& self = *static_cast<IsolatedHeap*>(context);
        return mx_arena_resize(self.arena_, bytes) == MX_OK ? bytes : 0;
    }
public:
    IsolatedHeap() = default;
    IsolatedHeap(const IsolatedHeap&) = delete;
    IsolatedHeap& operator=(const IsolatedHeap&) = delete;
    static consteval mx_u64_t type_id() {
        mx_u64_t hash = 1469598103934665603ull;
        for (char c : __PRETTY_FUNCTION__) { if (!c) break; hash = (hash ^ mx_u8_t(c)) * 1099511628211ull; }
        return hash ? hash : 1;
    }
    mx_status_t initialize(MxArenaCreate request) {
        Guard guard(lock_);
        if (heap_.initialized() || (request.type_id && request.type_id != type_id())) return MX_ERR_INVALID;
        request.type_id = type_id();
        auto status = mx_arena_managed_create(request, &arena_);
        if (status != MX_OK) return status;
        status = mx_map_page(arena_, request.cage_base, RIGHT_READ | RIGHT_WRITE);
        if (status != MX_OK) { (void)sys_cap_revoke(arena_); arena_ = 0; return status; }
        if (!heap_.initialize(reinterpret_cast<void*>(request.cage_base), request.size,
                              request.cage_size, this, grow)) {
            (void)mx_arena_unmap_cap(arena_, request.cage_base);
            (void)sys_cap_revoke(arena_); arena_ = 0; return MX_ERR_INVALID;
        }
        return MX_OK;
    }
    template <typename... Args> T* create(Args&&... args) {
        T* object;
        { Guard guard(lock_); object = static_cast<T*>(heap_.allocate_aligned(alignof(T), sizeof(T))); }
        return object ? new (object) T(static_cast<Args&&>(args)...) : nullptr;
    }
    bool destroy(T* object) {
        if (!object) return true;
        // Validate ownership before invoking a destructor supplied by the caller.
        { Guard guard(lock_); if (!heap_.owns(object)) return false; }
        object->~T();
        Guard guard(lock_); return heap_.release(object);
    }
};
}

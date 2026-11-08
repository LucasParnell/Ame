#pragma once
// mochi/arena.h — arena-backed allocation (no ambient heap).
//
// Per architecture §8, a service allocates writable memory from a registered
// arena, never from a global heap. An Arena is the data-oriented allocator over
// such a region: a bump pointer that hands out aligned blocks and is freed by
// rewinding. Optional pre-mapped capacity permits metadata-only resizing.
// Reallocating the last block changes the cursor; earlier blocks use a new bump
// block and retain their old storage until rewind. Requests beyond capacity fail.
//
// The region is caller-owned — typically a mapped CAP_ARENA — so this header is
// pure and host-testable (tests/test_arena.py).

#include "libk/integer.h"
#include "libk/bytes.h"
#include "midix/abi/types.h"

namespace mochi {

class Arena {
public:
    constexpr Arena() = default;
    Arena(void* base, mx_size_t size, mx_size_t capacity = 0)
        : base_(static_cast<mx_u8_t*>(base)), size_(base ? size : 0),
          capacity_(base ? (capacity > size ? capacity : size) : 0) {}

    bool valid() const { return base_ != nullptr; }
    void* base() const { return base_; }
    mx_size_t size() const { return size_; }
    mx_size_t used() const { return offset_; }
    mx_size_t remaining() const { return size_ - offset_; }
    mx_size_t capacity() const { return capacity_; }

    // Metadata-only: the caller guarantees that capacity is already mapped.
    // Managed kernel arenas use mx_arena_resize before enlarging this view.
    bool resize(mx_size_t size) {
        if (size < offset_ || size > capacity_) return false;
        size_ = size; return true;
    }

    // The supplied old_size must describe a live allocation. The last block
    // resizes in place without copying; an earlier block gets a new bump block.
    // Failure preserves the original block and allocation cursor.
    void* realloc(void* block, mx_size_t old_size, mx_size_t new_size, mx_size_t align = 16) {
        if (!block) return alloc(new_size, align);
        if (!base_) return nullptr;
        if (!align) align = 16;
        if (align & (align - 1)) return nullptr;
        const auto address = reinterpret_cast<mx_u64_t>(block);
        const auto base = reinterpret_cast<mx_u64_t>(base_);
        if (address < base || address - base > offset_) return nullptr;
        const mx_size_t start = address - base;
        if (old_size > offset_ - start) return nullptr;
        if (start + old_size == offset_ && !(address & (align - 1))) {
            if (new_size > size_ - start) return nullptr;
            if (!libk::checked_add(address, new_size, nullptr)) return nullptr;
            offset_ = start + new_size;
            if (offset_ > high_water_) high_water_ = offset_;
            return new_size ? block : nullptr;
        }
        if (!new_size) return nullptr;
        if (new_size <= old_size && !(address & (align - 1))) return block;
        auto* replacement = alloc(new_size, align);
        if (!replacement) return nullptr;
        libk::copy_n(libk::as_bytes(replacement, new_size), libk::as_bytes(block, old_size),
                     old_size < new_size ? old_size : new_size);
        return replacement;
    }

    // Allocate `size` bytes aligned to `align` (a power of two; 0 means 16).
    // Returns nullptr when the request cannot be satisfied: the arena never
    // grows and never overruns. The block lives until reset().
    void* alloc(mx_size_t size, mx_size_t align = 16) {
        if (!base_) return nullptr;
        if (align == 0) align = 16;
        // Align the address, not only the offset: callers may provide a slice
        // whose base does not already have the requested alignment.
        mx_u64_t address = 0;
        mx_u64_t aligned = 0;
        if (!libk::checked_add(reinterpret_cast<mx_u64_t>(base_), offset_, &address) ||
            !libk::checked_align_up(address, align, &aligned)) return nullptr;
        const mx_size_t padding = aligned - address;
        if (padding > remaining()) return nullptr;
        const mx_size_t start = offset_ + padding;
        if (start > size_ || size > size_ - start) return nullptr;
        if (!libk::checked_add(aligned, size, nullptr)) return nullptr;
        offset_ = start + size;
        if (offset_ > high_water_) high_water_ = offset_;
        return base_ + start;
    }

    // Typed allocation of `count` elements; rejects a count that would overflow
    // the size multiplication.
    template <typename T>
    T* alloc(mx_size_t count = 1, mx_size_t align = alignof(T)) {
        if (count == 0 || count > (~static_cast<mx_size_t>(0)) / sizeof(T)) return nullptr;
        if (align == 0) align = 16;
        if ((align & (align - 1)) != 0) return nullptr;
        if (align < alignof(T)) align = alignof(T);
        return static_cast<T*>(alloc(sizeof(T) * count, align));
    }

    // DMA queues and protocol buffers require zeroed storage, including padding.
    void* alloc_zeroed(mx_size_t size, mx_size_t align = 16) {
        void* block = alloc(size, align);
        if (block) libk::zero(block, size);
        return block;
    }

    // Rewind to empty. Keeps base/size and the high-water mark.
    void reset() { offset_ = 0; }
    mx_size_t high_water() const { return high_water_; }

private:
    mx_u8_t* base_ = nullptr;
    mx_size_t size_ = 0;
    mx_size_t capacity_ = 0;
    mx_size_t offset_ = 0;
    mx_size_t high_water_ = 0;
};

} // namespace mochi

#pragma once
// Pure allocator over a private process arena. Allocation/free stay local;
// backing growth is the only provider operation. Geometric free bins make
// the common selection O(1); a fragmented final bin is searched under pressure.
#include "midix/abi/types.h"
#include "libk/bytes.h"

namespace mochi::memory {
class Heap {
    static constexpr mx_u32_t nil = ~mx_u32_t{0};
    static constexpr mx_u64_t magic = 0x4d4f434849484541ull;
    struct alignas(16) Block {
        mx_u64_t tag;
        mx_u32_t size, previous, requested, used, next, prior;
    };
    struct Aligned { mx_u64_t tag, raw, requested, alignment; };
    static_assert(sizeof(Block) == 32);
    mx_u8_t* base_ = nullptr;
    mx_u32_t bytes_ = 0, limit_ = 0, heads_[32]{};
    mx_u32_t bins_ = 0;
    mx_u64_t live_ = 0;
    mx_u32_t quarantine_[16]{}, quarantine_head_ = 0, quarantine_count_ = 0;
    void* context_ = nullptr;
    mx_u64_t (*grow_)(void*, mx_u64_t) = nullptr;
    static unsigned order(mx_u32_t size) { return 31u - __builtin_clz(size); }
    Block* block(mx_u32_t offset) const { return reinterpret_cast<Block*>(base_ + offset); }
    mx_u32_t offset(const Block* b) const { return reinterpret_cast<const mx_u8_t*>(b) - base_; }
    mx_u64_t tag(mx_u32_t off) const { return magic ^ reinterpret_cast<mx_u64_t>(base_) ^ off; }
    mx_u64_t signature(const Block& b) const {
        return tag(offset(&b)) ^ (mx_u64_t{b.size} << 32) ^ b.requested ^ (mx_u64_t{b.used} << 61);
    }
    void stamp(Block& b) { b.tag = signature(b); }
    void require(bool condition) const { if (!condition) __builtin_trap(); }
    Block* checked(mx_u32_t off) const {
        require(off <= bytes_ - sizeof(Block) && !(off & 15));
        auto* b = block(off); require(valid(b)); return b;
    }
    void guard(mx_u8_t* p, mx_u64_t value) const {
        for (unsigned i = 0; i < 8; ++i) p[i] = value >> (8 * i);
    }
    bool guarded(const mx_u8_t* p, mx_u64_t value) const {
        for (unsigned i = 0; i < 8; ++i) if (p[i] != mx_u8_t(value >> (8 * i))) return false;
        return true;
    }
    bool valid(const Block* b) const {
        const auto off = offset(b);
        return off <= bytes_ - sizeof(Block) && !(off & 15) && b->tag == signature(*b) &&
               (!b->size || (b->size >= 64 && !(b->size & 15) && b->size <= bytes_ - off - sizeof(Block)));
    }
    void insert(Block& b) {
        const auto bin = order(b.size), off = offset(&b);
        b.next = heads_[bin]; b.prior = nil; b.used = 0; stamp(b);
        if (b.next != nil) { auto* next = checked(b.next); require(!next->used); next->prior = off; }
        heads_[bin] = off; bins_ |= 1u << bin;
    }
    void remove(Block& b) {
        require(valid(&b) && !b.used && b.size);
        const auto bin = order(b.size), off = offset(&b);
        Block* prior = b.prior == nil ? nullptr : checked(b.prior);
        Block* next = b.next == nil ? nullptr : checked(b.next);
        require(prior ? !prior->used && prior->next == off && order(prior->size) == bin : heads_[bin] == off);
        require(!next || (!next->used && next->prior == off && order(next->size) == bin));
        if (prior) prior->next = b.next; else heads_[bin] = b.next;
        if (next) next->prior = b.prior;
        if (heads_[bin] == nil) bins_ &= ~(1u << bin);
    }
    Block* find(mx_u32_t needed) const {
        const auto index = order(needed);
        auto entry = heads_[index];
        for (unsigned attempt = 0; entry != nil && attempt < 4; ++attempt) {
            auto* candidate = checked(entry); require(!candidate->used && order(candidate->size) == index);
            if (candidate->size >= needed) return candidate;
            entry = candidate->next;
        }
        const auto larger = index < 31 ? bins_ & (~mx_u32_t{0} << (index + 1)) : 0;
        if (larger) {
            const auto bin = __builtin_ctz(larger); auto* b = checked(heads_[bin]);
            require(!b->used && order(b->size) == unsigned(bin) && b->size >= needed); return b;
        }
        mx_u32_t traversed = 0;
        while (entry != nil) {
            require(++traversed <= bytes_ / 64);
            auto* candidate = checked(entry); require(!candidate->used && order(candidate->size) == index);
            if (candidate->size >= needed) return candidate;
            entry = candidate->next;
        }
        return nullptr;
    }
    void split(Block& b, mx_u32_t size) {
        const auto rest = b.size - size;
        if (rest < 64) return;
        b.size = size; stamp(b);
        auto& tail = *block(offset(&b) + size);
        tail = {0, rest, size, 0, 0, nil, nil}; stamp(tail);
        auto* next = checked(offset(&tail) + rest);
        if (next->size && !next->used) {
            remove(*next); tail.size += next->size; next->tag = 0;
        }
        block(offset(&tail) + tail.size)->previous = tail.size;
        insert(tail);
    }
    bool expand(mx_u32_t minimum) {
        if (!grow_ || bytes_ == limit_) return false;
        mx_u64_t wanted = mx_u64_t{bytes_} + minimum + sizeof(Block);
        if (wanted < 2ull * bytes_) wanted = 2ull * bytes_;
        if (wanted > limit_) wanted = limit_;
        wanted = (wanted + 4095) & ~mx_u64_t{4095};
        if (wanted > limit_ || wanted <= bytes_) return false;
        const auto actual = grow_(context_, wanted);
        if (actual != wanted) return false;
        auto* tail = checked(bytes_ - sizeof(Block));
        const auto additional = mx_u32_t(actual) - bytes_;
        tail->size = additional; tail->used = 0;
        const auto previous = tail->previous;
        bytes_ = actual;
        auto& sentinel = *block(bytes_ - sizeof(Block));
        sentinel = {0, 0, additional, 0, 1, nil, nil}; stamp(sentinel);
        if (previous) {
            auto* left = checked(offset(tail) - previous);
            if (!left->used) {
                remove(*left); left->size += additional; tail->tag = 0;
                tail = left; sentinel.previous = left->size;
            }
        }
        insert(*tail); return true;
    }
    Block* allocation(void* pointer) const {
        if (!base_ || !pointer) return nullptr;
        const auto address = reinterpret_cast<mx_u64_t>(pointer);
        const auto base = reinterpret_cast<mx_u64_t>(base_);
        if (address < base + sizeof(Block) || address >= base + bytes_ || (address & 15)) return nullptr;
        auto* b = block(address - base - sizeof(Block));
        if (!valid(b) || b->used != 1 || !b->size || b->requested > b->size - sizeof(Block) - 8 ||
            !guarded(static_cast<mx_u8_t*>(pointer) + b->requested, b->tag ^ b->requested)) return nullptr;
        return b;
    }
    Aligned* aligned(void* pointer) const {
        if (!base_ || !pointer) return nullptr;
        const auto address = reinterpret_cast<mx_u64_t>(pointer), base = reinterpret_cast<mx_u64_t>(base_);
        if (address < base + sizeof(Block) + sizeof(Aligned) || address >= base + bytes_ || (address & 15)) return nullptr;
        auto* prefix = reinterpret_cast<Aligned*>(address - sizeof(Aligned));
        if (prefix->tag != (magic ^ address) || prefix->raw >= bytes_ || !prefix->alignment ||
            (prefix->alignment & (prefix->alignment - 1)) || (address & (prefix->alignment - 1))) return nullptr;
        auto* raw = allocation(base_ + prefix->raw);
        if (!raw || address < base + prefix->raw ||
            address - (base + prefix->raw) > raw->requested ||
            prefix->requested > raw->requested ||
            raw->requested - prefix->requested < 8 ||
            address - (base + prefix->raw) > raw->requested - prefix->requested - 8 ||
            !guarded(static_cast<mx_u8_t*>(pointer) + prefix->requested, prefix->tag ^ prefix->requested)) return nullptr;
        return prefix;
    }
public:
    bool initialize(void* base, mx_u64_t bytes, mx_u64_t limit, void* context = nullptr,
                    mx_u64_t (*grow)(void*, mx_u64_t) = nullptr) {
        if (base_ || !base || (reinterpret_cast<mx_u64_t>(base) & 4095) || bytes < 4096 ||
            bytes > limit || limit > 0x40000000 || ((bytes | limit) & 4095)) return false;
        base_ = static_cast<mx_u8_t*>(base); bytes_ = bytes; limit_ = limit; context_ = context; grow_ = grow;
        for (auto& head : heads_) head = nil;
        auto& first = *block(0); first = {0, bytes_ - mx_u32_t(sizeof(Block)), 0, 0, 0, nil, nil};
        auto& sentinel = *block(first.size); sentinel = {0, 0, first.size, 0, 1, nil, nil}; stamp(sentinel);
        insert(first); return true;
    }
    bool initialized() const { return base_ != nullptr; }
    bool owns(void* pointer) const { return allocation(pointer) || aligned(pointer); }
    mx_u64_t committed() const { return bytes_; }
    mx_u64_t live_bytes() const { return live_; }
    void* allocate(mx_u64_t size) {
        if (!base_ || size > limit_ || size > 0x20000000) return nullptr;
        const auto total = mx_u32_t(size + sizeof(Block) + 8);
        const mx_u32_t needed = total <= 64 ? 64 : (total + 15) & ~mx_u32_t{15};
        auto* available = find(needed);
        while (!available && quarantine_count_) { drain_one(); available = find(needed); }
        if (!available) {
            if (!expand(needed)) return nullptr;
            available = find(needed);
            if (!available) return nullptr;
        }
        auto& b = *available;
        remove(b); split(b, needed); b.used = 1; b.requested = size; stamp(b); live_ += size;
        auto* result = reinterpret_cast<mx_u8_t*>(&b) + sizeof(Block);
        guard(result + size, b.tag ^ size); return result;
    }
    bool release(void* pointer) {
        if (!pointer) return true;
        auto* b = allocation(pointer);
        if (!b) {
            auto* prefix = aligned(pointer);
            return prefix ? release(base_ + prefix->raw) : false;
        }
        live_ -= b->requested;
        libk::zero(pointer, b->requested);
        b->used = 2; stamp(*b);
        guard(static_cast<mx_u8_t*>(pointer) + b->requested, b->tag ^ b->requested);
        if (quarantine_count_ == 16) drain_one();
        quarantine_[(quarantine_head_ + quarantine_count_) % 16] = offset(b); ++quarantine_count_;
        return true;
    }
private:
    void drain_one() {
        auto* b = checked(quarantine_[quarantine_head_]);
        quarantine_head_ = (quarantine_head_ + 1) % 16; --quarantine_count_;
        require(b->used == 2 && b->requested <= b->size - sizeof(Block) - 8);
        auto* payload = reinterpret_cast<mx_u8_t*>(b) + sizeof(Block);
        for (mx_u32_t i = 0; i < b->requested; ++i) require(payload[i] == 0);
        require(guarded(payload + b->requested, b->tag ^ b->requested));
        auto* right = checked(offset(b) + b->size);
        Block* left = nullptr;
        if (b->previous) {
            require(b->previous <= offset(b)); left = checked(offset(b) - b->previous);
            require(left->size == b->previous);
        }
        if (left && !left->used) { remove(*left); left->size += b->size; b->tag = 0; b = left; }
        if (right->size && !right->used) { remove(*right); b->size += right->size; right->tag = 0; }
        checked(offset(b) + b->size)->previous = b->size;
        insert(*b);
    }
public:
    void* allocate_aligned(mx_u64_t alignment, mx_u64_t size) {
        if (alignment < 16) alignment = 16;
        if (alignment & (alignment - 1) || alignment > limit_ || alignment + sizeof(Aligned) + 8 > limit_ ||
            size > limit_ - alignment - sizeof(Aligned) - 8)
            return nullptr;
        auto* raw = static_cast<mx_u8_t*>(allocate(size + alignment + sizeof(Aligned) + 8));
        if (!raw) return nullptr;
        const auto address = (reinterpret_cast<mx_u64_t>(raw) + sizeof(Aligned) + alignment - 1) & ~(alignment - 1);
        auto* prefix = reinterpret_cast<Aligned*>(address - sizeof(Aligned));
        *prefix = {magic ^ address, mx_u64_t(raw - base_), size, alignment};
        guard(reinterpret_cast<mx_u8_t*>(address) + size, prefix->tag ^ size);
        return reinterpret_cast<void*>(address);
    }
    void* reallocate(void* pointer, mx_u64_t size) {
        if (!pointer) return allocate(size);
        if (!size) { (void)release(pointer); return nullptr; }
        auto* b = allocation(pointer);
        if (!b) {
            auto* prefix = aligned(pointer);
            if (!prefix) return nullptr;
            auto* result = allocate_aligned(prefix->alignment, size);
            if (result) {
                libk::copy_n(libk::as_bytes(result, size), libk::as_bytes(pointer, prefix->requested),
                             size < prefix->requested ? size : prefix->requested);
                (void)release(pointer);
            }
            return result;
        }
        if (size > limit_ || size > 0x20000000) return nullptr;
        const auto total = mx_u32_t(size + sizeof(Block) + 8);
        const auto needed = total <= 64 ? 64 : (total + 15) & ~mx_u32_t{15};
        auto* right = checked(offset(b) + b->size);
        if (!valid(right)) return nullptr;
        if (needed > b->size && ((!right->size) || (!right->used &&
            !checked(offset(right) + right->size)->size)) && needed > b->size + right->size) {
            if (expand(needed - b->size - right->size)) right = block(offset(b) + b->size);
        }
        if (needed > b->size && right->size && !right->used && needed <= b->size + right->size) {
            remove(*right); b->size += right->size; right->tag = 0;
            checked(offset(b) + b->size)->previous = b->size; stamp(*b);
        }
        if (needed <= b->size) {
            live_ = live_ - b->requested + size; b->requested = size; stamp(*b); split(*b, needed);
            guard(static_cast<mx_u8_t*>(pointer) + size, b->tag ^ size); return pointer;
        }
        const auto previous = b->requested;
        auto* result = allocate(size);
        if (!result) return nullptr;
        libk::copy_n(libk::as_bytes(result, size), libk::as_bytes(pointer, previous), previous);
        (void)release(pointer); return result;
    }
};
}

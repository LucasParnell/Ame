#include "anko/object.h"
#include "anko/mm.h"
#include "anko/spinlock.h"
#include "anko/panic.h"

namespace object {
struct Record {
    void* target;
    Destructor destroy;
    u64 references;
    Record* next;
};
static kernel::SpinLock lock{};
// Cached references bypass this registry entirely. Cold creation/lookup/final
// unlink use a compact fixed bucket index, keeping unrelated objects out of
// each traversal without growing the 32-byte lifetime record or allocating a
// second index node. The registry lock retains the existing reclamation rule.
static_assert(sizeof(Record) == 32);
static constexpr u32 kBucketBits = 8;
alignas(64) static Record* objects[1u << kBucketBits]{};
static u64 count;

u64 reference_count(Record* record) {
    return record ? __atomic_load_n(&record->references, __ATOMIC_ACQUIRE) : 0;
}

static Record*& bucket(void* target) {
    u64 address = reinterpret_cast<usize>(target) >> 3;
    address ^= address >> 17;
    return objects[(address * 0x9e3779b97f4a7c15ull) >> (64 - kBucketBits)];
}

Record* create(void* target, Destructor destroy) {
    if (!target) {
        return nullptr;
    }
    auto* fresh = static_cast<Record*>(mm::slab_alloc(sizeof(Record), alignof(Record)));
    if (!fresh) {
        return nullptr;
    }
    *fresh = {target, destroy, 1, nullptr};
    kernel::LockGuard guard(lock);
    auto& head = bucket(target);
    for (auto* p = head; p; p = p->next) {
        ANKO_ASSERT(p->target != target);
    }
    fresh->next = head;
    head = fresh;
    ++count;
    return fresh;
}

Record* acquire(void* target) {
    if (!target) {
        return nullptr;
    }
    kernel::LockGuard guard(lock);
    auto& head = bucket(target);
    for (auto* p = head; p; p = p->next) {
        if (p->target == target) {
            // Final release can reach zero before it acquires the registry
            // lock to unlink. Never resurrect that retiring record.
            u64 refs = __atomic_load_n(&p->references, __ATOMIC_RELAXED);
            while (refs) {
                ANKO_ASSERT(refs != ~0ull);
                if (__atomic_compare_exchange_n(&p->references, &refs, refs + 1,
                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return p;
            }
            return nullptr;
        }
    }
    auto* fresh = static_cast<Record*>(mm::slab_alloc(sizeof(Record), alignof(Record)));
    if (!fresh) {
        return nullptr;
    }
    *fresh = {target, nullptr, 1, head};
    head = fresh;
    ++count;
    return fresh;
}

void retain(Record* record) {
    if (!record) {
        return;
    }
    // Caller owns a reference already, so zero/final destruction cannot race
    // this increment. Cached scheduler references never touch the registry.
    const u64 previous = __atomic_fetch_add(&record->references, 1, __ATOMIC_RELAXED);
    ANKO_ASSERT(previous && previous != ~0ull);
}

void release(Record* record) {
    if (!record) {
        return;
    }
    // Release publishes preceding object use. Only the final owner needs the
    // acquire fence and registry unlink; routine drops remain lock-free.
    const u64 previous = __atomic_fetch_sub(&record->references, 1, __ATOMIC_RELEASE);
    ANKO_ASSERT(previous);
    if (previous != 1) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    {
        kernel::LockGuard guard(lock);
        Record** link = &bucket(record->target);
        while (*link && *link != record) {
            link = &(*link)->next;
        }
        ANKO_ASSERT(*link == record);
        *link = record->next;
        --count;
    }
    if (record->destroy) {
        record->destroy(record->target);
    }
    mm::slab_free(record, sizeof(Record), alignof(Record));
}

u64 live_count() {
    kernel::LockGuard guard(lock);
    return count;
}
} // namespace object

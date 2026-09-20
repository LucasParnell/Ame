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
static Record* objects;
static u64 count;

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
    for (auto* p = objects; p; p = p->next) {
        ANKO_ASSERT(p->target != target);
    }
    fresh->next = objects;
    objects = fresh;
    ++count;
    return fresh;
}

Record* acquire(void* target) {
    if (!target) {
        return nullptr;
    }
    kernel::LockGuard guard(lock);
    for (auto* p = objects; p; p = p->next) {
        if (p->target == target) {
            ++p->references;
            return p;
        }
    }
    auto* fresh = static_cast<Record*>(mm::slab_alloc(sizeof(Record), alignof(Record)));
    if (!fresh) {
        return nullptr;
    }
    *fresh = {target, nullptr, 1, objects};
    objects = fresh;
    ++count;
    return fresh;
}

void retain(Record* record) {
    if (!record) {
        return;
    }
    kernel::LockGuard guard(lock);
    ANKO_ASSERT(record->references);
    ++record->references;
}

void release(Record* record) {
    if (!record) {
        return;
    }
    {
        kernel::LockGuard guard(lock);
        ANKO_ASSERT(record->references);
        if (--record->references) {
            return;
        }
        Record** link = &objects;
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

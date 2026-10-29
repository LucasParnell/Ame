#pragma once
#include "anko/types.h"

// A reference protects the object, independently of the capability slot that
// supplied it. Retaining an owned record and dropping a nonfinal reference are
// lock-free. Creation, target lookup, and final unlink use the registry lock;
// destructors run after that lock is released.
namespace object {
struct Record;
using Destructor = void (*)(void*);
Record* create(void* target, Destructor destroy); // one creator reference
Record* acquire(void* target);                    // static objects are allowed
void retain(Record* record);
void release(Record* record);
u64 live_count();
// Caller holds a reference. Includes capability and mapping pins, so an arena
// cache can refuse reuse while read-only client views still exist.
u64 reference_count(Record* record);

template <class T> class Ref {
    T* target_ = nullptr;
    Record* record_ = nullptr;

  public:
    Ref() = default;
    Ref(decltype(nullptr)) {}
    explicit Ref(T* target) : target_(target), record_(acquire(target)) {
        if (!record_) {
            target_ = nullptr;
        }
    }
    // The caller already owns a live reference and has cached its record.
    // Retain that identity directly instead of searching the object registry.
    // A missing record preserves support for caller-owned/static probe objects.
    Ref(T* target, Record* record) : target_(target), record_(record) {
        if (record_) retain(record_);
        else record_ = acquire(target);
        if (!record_) target_ = nullptr;
    }
    Ref(const Ref& other) : target_(other.target_), record_(other.record_) {
        retain(record_);
    }
    Ref(Ref&& other) : target_(other.target_), record_(other.record_) {
        other.target_ = nullptr;
        other.record_ = nullptr;
    }
    ~Ref() {
        release(record_);
    }
    Ref& operator=(const Ref&) = delete;
    void reset(T* target = nullptr) {
        Record* fresh = acquire(target);
        auto* old = record_;
        record_ = fresh;
        target_ = fresh ? target : nullptr;
        release(old);
    }
    void reset(T* target, Record* record) {
        // Retain before dropping the old reference, including self-reset.
        if (record) retain(record);
        else record = acquire(target);
        auto* old = record_;
        record_ = record;
        target_ = record ? target : nullptr;
        release(old);
    }
    T* get() const {
        return target_;
    }
    operator T*() const {
        return target_;
    }
    T* operator->() const {
        return target_;
    }
    T& operator*() const {
        return *target_;
    }
};
} // namespace object

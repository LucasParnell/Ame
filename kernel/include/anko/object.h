#pragma once
#include "anko/types.h"

// A reference protects the object, independently of the capability slot that
// supplied it. Destructors run after the object registry lock is released.
namespace object {
struct Record;
using Destructor = void (*)(void*);
Record* create(void* target, Destructor destroy); // one creator reference
Record* acquire(void* target);                    // static objects are allowed
void retain(Record* record);
void release(Record* record);
u64 live_count();

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

#pragma once
#include "midix/usb/xhci_ring.h"

namespace midix::usb::xhci {

// Preserve unrelated events during a cold command/control wait. Storage is
// supplied by the controller's CPU arena, separate from device DMA queues.
class EventBacklog {
public:
    EventBacklog() = default;
    EventBacklog(const EventBacklog&) = delete;
    EventBacklog& operator=(const EventBacklog&) = delete;
    bool init(Trb* storage, mx_u32_t capacity) {
        if (!storage || !capacity) return false;
        storage_ = storage; capacity_ = capacity; head_ = tail_ = count_ = 0;
        return true;
    }
    bool push(const Trb& event) {
        if (count_ == capacity_) return false;
        storage_[head_] = event;
        if (++head_ == capacity_) head_ = 0;
        ++count_;
        return true;
    }
    bool pop(Trb* out) {
        if (!out || !count_) return false;
        *out = storage_[tail_];
        if (++tail_ == capacity_) tail_ = 0;
        --count_;
        return true;
    }
    mx_u32_t depth() const { return count_; }
private:
    Trb* storage_ = nullptr;
    mx_u32_t capacity_ = 0;
    mx_u32_t head_ = 0, tail_ = 0, count_ = 0;
};

} // namespace midix::usb::xhci

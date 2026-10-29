#pragma once
// Caller-backed, single-owner slot allocation. No heap and no object graph.
#include <cstdint>

namespace libk {

constexpr std::uint32_t kNoSlot = ~std::uint32_t{0};
struct SlotMetadata {
    std::uint32_t generation;
    std::uint32_t next;
    bool allocated;
};

class SlotPool {
public:
    SlotPool() = default;
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;
    bool init(SlotMetadata* slots, std::uint32_t count) {
        if (!slots || !count || count == kNoSlot) return false;
        slots_ = slots;
        count_ = count;
        first_ = 0;
        free_ = count;
        for (std::uint32_t i = 0; i < count; ++i)
            slots_[i] = {1, i + 1 == count ? kNoSlot : i + 1, false};
        return true;
    }

    std::uint64_t acquire() {
        if (first_ == kNoSlot) return 0;
        const auto index = first_;
        auto& slot = slots_[index];
        first_ = slot.next;
        slot.next = kNoSlot;
        slot.allocated = true;
        --free_;
        return (static_cast<std::uint64_t>(slot.generation) << 32) | (index + 1);
    }

    std::uint32_t index_of(std::uint64_t token) const {
        const auto low = static_cast<std::uint32_t>(token);
        if (!low || low > count_) return kNoSlot;
        const auto index = low - 1;
        const auto& slot = slots_[index];
        return slot.allocated && slot.generation == token >> 32 ? index : kNoSlot;
    }

    bool release(std::uint64_t token) {
        const auto index = index_of(token);
        if (index == kNoSlot) return false;
        auto& slot = slots_[index];
        slot.allocated = false;
        // Retire on generation exhaustion: an old token can never become valid.
        if (++slot.generation == 0) return true;
        slot.next = first_;
        first_ = index;
        ++free_;
        return true;
    }

    std::uint64_t token_at(std::uint32_t index) const {
        if (index >= count_ || !slots_[index].allocated) return 0;
        return (static_cast<std::uint64_t>(slots_[index].generation) << 32) | (index + 1);
    }
    std::uint32_t capacity() const { return count_; }
    std::uint32_t available() const { return free_; }

private:
    SlotMetadata* slots_ = nullptr;
    std::uint32_t count_ = 0;
    std::uint32_t first_ = kNoSlot;
    std::uint32_t free_ = 0;
};

} // namespace libk

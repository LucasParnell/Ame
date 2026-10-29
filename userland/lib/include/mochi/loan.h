#pragma once
// Single-thread-owned, bounded shared-buffer loans. Transport descriptors and
// returns over an ordered channel; its release/acquire publication orders the
// payload. No shared allocator state, allocation or syscall on these paths.
#include "libk/bytes.h"
#include "midix/abi/status.h"
#include <cstdint>

namespace mochi {
struct LoanDescriptor {
    uint64_t pool, generation;
    uint32_t slot, size;
};
static_assert(sizeof(LoanDescriptor) == 24);
struct LoanMemory {
    const void* data = nullptr;
    uint32_t count = 0, stride = 0;
    uint64_t cookie = 0;
    bool valid() const {
        return data && count && count <= 64 && stride && stride <= 65536 && !(stride & 63) &&
               cookie;
    }
    uint64_t bytes() const {
        return uint64_t{count} * stride;
    }
};
class LoanWriter;
class LoanReader;
class WriteLoan {
    friend class LoanWriter;
    LoanWriter* owner_ = nullptr;
    LoanDescriptor descriptor_{};

  public:
    WriteLoan() = default;
    WriteLoan(const WriteLoan&) = delete;
    WriteLoan& operator=(const WriteLoan&) = delete;
    WriteLoan(WriteLoan&& other) : owner_(other.owner_), descriptor_(other.descriptor_) {
        other.owner_ = nullptr;
    }
    ~WriteLoan() {
        cancel();
    }
    bool valid() const {
        return owner_ != nullptr;
    }
    libk::bytes bytes() const;
    // Ownership leaves this object before transport publication. If sending
    // fails ambiguously, keep the slot in flight until channel teardown.
    mx_status_t publish(LoanDescriptor& out);
    void cancel();
};
class ReadLoan {
    friend class LoanReader;
    LoanReader* owner_ = nullptr;
    LoanDescriptor descriptor_{};

  public:
    ReadLoan() = default;
    ReadLoan(const ReadLoan&) = delete;
    ReadLoan& operator=(const ReadLoan&) = delete;
    ReadLoan(ReadLoan&& other) : owner_(other.owner_), descriptor_(other.descriptor_) {
        other.owner_ = nullptr;
    }
    // Dropping without release quarantines the slot: it does not silently
    // return ownership or perform blocking I/O from a destructor.
    bool valid() const {
        return owner_ != nullptr;
    }
    libk::const_bytes bytes() const;
    mx_status_t release(LoanDescriptor& out);
};

class LoanWriter {
    friend class WriteLoan;
    LoanMemory memory_;
    void* writable_;
    uint64_t writing_ = 0, in_flight_ = 0;
    uint64_t generation_[64]{};
    uint32_t length_[64]{};
    bool matches(const LoanDescriptor& d) const {
        return memory_.valid() && d.pool == memory_.cookie && d.slot < memory_.count &&
               d.generation && d.generation == generation_[d.slot] && d.size == length_[d.slot];
    }

  public:
    LoanWriter(void* data, uint32_t count, uint32_t stride, uint64_t cookie)
        : memory_{data, count, stride, cookie}, writable_(data) {
    }
    LoanWriter(const LoanWriter&) = delete;
    LoanWriter& operator=(const LoanWriter&) = delete;
    bool valid() const {
        return memory_.valid();
    }
    mx_status_t acquire(uint32_t size, WriteLoan& out) {
        if (!valid() || size > memory_.stride || out.valid())
            return MX_ERR_INVALID;
        for (uint32_t i = 0; i < memory_.count; ++i) {
            const uint64_t bit = uint64_t{1} << i;
            if (((writing_ | in_flight_) & bit) || generation_[i] == ~uint64_t{0})
                continue;
            writing_ |= bit;
            length_[i] = size;
            out.descriptor_ = {memory_.cookie, ++generation_[i], i, size};
            out.owner_ = this;
            return MX_OK;
        }
        return MX_ERR_WOULD_BLOCK;
    }
    // A returned loan can be reacquired for reply validation before reuse.
    mx_status_t reclaim(const LoanDescriptor& d, WriteLoan* out = nullptr) {
        if ((out && out->valid()) || !matches(d))
            return MX_ERR_BAD_PROTOCOL;
        const uint64_t bit = uint64_t{1} << d.slot;
        if (!(in_flight_ & bit))
            return MX_ERR_BAD_PROTOCOL;
        if (out && generation_[d.slot] == ~uint64_t{0})
            return MX_ERR_OVERFLOW;
        in_flight_ &= ~bit;
        if (out) {
            writing_ |= bit;
            out->owner_ = this;
            out->descriptor_ = d;
            out->descriptor_.generation = ++generation_[d.slot];
        }
        return MX_OK;
    }
    bool drained() const {
        return !(writing_ | in_flight_);
    }
};
inline libk::bytes WriteLoan::bytes() const {
    return owner_ ? libk::as_bytes(static_cast<uint8_t*>(owner_->writable_) +
                                       uint64_t{descriptor_.slot} * owner_->memory_.stride,
                                   descriptor_.size)
                  : libk::bytes{};
}
inline mx_status_t WriteLoan::publish(LoanDescriptor& out) {
    if (!owner_)
        return MX_ERR_INVALID;
    const uint64_t bit = uint64_t{1} << descriptor_.slot;
    owner_->writing_ &= ~bit;
    owner_->in_flight_ |= bit;
    out = descriptor_;
    owner_ = nullptr;
    return MX_OK;
}
inline void WriteLoan::cancel() {
    if (owner_)
        owner_->writing_ &= ~(uint64_t{1} << descriptor_.slot);
    owner_ = nullptr;
}

class LoanReader {
    friend class ReadLoan;
    LoanMemory memory_;
    uint64_t held_ = 0, generation_[64]{};

  public:
    explicit LoanReader(LoanMemory memory) : memory_(memory) {
    }
    LoanReader(const LoanReader&) = delete;
    LoanReader& operator=(const LoanReader&) = delete;
    bool valid() const {
        return memory_.valid();
    }
    mx_status_t accept(LoanDescriptor d, ReadLoan& out) {
        if (!valid() || out.valid() || d.pool != memory_.cookie || d.slot >= memory_.count ||
            d.size > memory_.stride || !d.generation)
            return MX_ERR_BAD_PROTOCOL;
        const uint64_t bit = uint64_t{1} << d.slot;
        if ((held_ & bit) || d.generation <= generation_[d.slot])
            return MX_ERR_BAD_PROTOCOL;
        held_ |= bit;
        generation_[d.slot] = d.generation;
        out.owner_ = this;
        out.descriptor_ = d;
        return MX_OK;
    }
    bool drained() const {
        return !held_;
    }
};
inline libk::const_bytes ReadLoan::bytes() const {
    return owner_ ? libk::as_bytes(static_cast<const uint8_t*>(owner_->memory_.data) +
                                       uint64_t{descriptor_.slot} * owner_->memory_.stride,
                                   descriptor_.size)
                  : libk::const_bytes{};
}
inline mx_status_t ReadLoan::release(LoanDescriptor& out) {
    if (!owner_)
        return MX_ERR_INVALID;
    owner_->held_ &= ~(uint64_t{1} << descriptor_.slot);
    out = descriptor_;
    owner_ = nullptr;
    return MX_OK;
}
} // namespace mochi

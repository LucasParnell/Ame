#pragma once
// A borrowed byte run with explicit accepted-prefix progress. Storage remains
// caller-owned and must stay stable until empty(); advance never wraps or moves
// past it. No allocation, IPC, clock or device policy belongs in this view.
#include "libk/bytes.h"
namespace libk {
struct ByteCursor {
  private:
    const_bytes bytes{};
    std::size_t position = 0;

  public:
    void reset(const_bytes next = {}) {
        bytes = next.data ? next : const_bytes{};
        position = 0;
    }
    std::size_t remaining() const {
        return bytes.size - position;
    }
    bool empty() const {
        return remaining() == 0;
    }
    const_bytes next(std::size_t limit) const {
        const auto count = remaining() < limit ? remaining() : limit;
        return slice(bytes, position, count);
    }
    bool advance(std::size_t accepted) {
        if (accepted > remaining()) {
            return false;
        }
        position += accepted;
        return true;
    }
};
} // namespace libk

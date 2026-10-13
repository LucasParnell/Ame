#pragma once
// Bounded volatile register access. The caller owns the mapping/capability and
// supplies hardware-specific ordering; this view performs no hidden barriers.
#include <cstddef>
#include <cstdint>
#include "libk/bytes.h"
#include "libk/endian.h"

namespace libk {

struct Mmio {
    volatile std::uint8_t* base = nullptr;
    std::size_t size = 0;

    bool read32(std::size_t offset, std::uint32_t* out) const {
        if (!out || !valid(offset, sizeof(std::uint32_t))) return false;
        *out = *reinterpret_cast<volatile std::uint32_t*>(base + offset);
        return true;
    }
    bool write32(std::size_t offset, std::uint32_t value) const {
        if (!valid(offset, sizeof(value))) return false;
        *reinterpret_cast<volatile std::uint32_t*>(base + offset) = value;
        return true;
    }
    bool write64(std::size_t offset, std::uint64_t value) const {
        if (!valid(offset, sizeof(value))) return false;
        *reinterpret_cast<volatile std::uint64_t*>(base + offset) = value;
        return true;
    }

    // A FIFO repeats accesses to ONE register, unlike a register array. Each
    // access consumes/publishes a complete little-endian word. Check the whole
    // CPU view before the first side effect and copy only the requested tail.
    // Ordering and FIFO space/count checks remain the hardware owner's job.
    bool read_fifo32(std::size_t offset, bytes destination) const {
        if (!valid(offset, sizeof(std::uint32_t)) ||
            (destination.size && !destination.data)) return false;
        for (std::size_t at = 0; at < destination.size;) {
            std::uint32_t value = 0;
            if (!read32(offset, &value)) return false;
            std::uint8_t word[4]{};
            write_le<std::uint32_t>(as_bytes(word), value);
            const auto length = destination.size - at < 4 ? destination.size - at : 4;
            if (!copy_n(slice(destination, at, length), as_bytes(word), length)) return false;
            at += length;
        }
        return true;
    }

    bool write_fifo32(std::size_t offset, const_bytes source) const {
        if (!valid(offset, sizeof(std::uint32_t)) || (source.size && !source.data)) return false;
        for (std::size_t at = 0; at < source.size;) {
            std::uint8_t word[4]{};
            const auto length = source.size - at < 4 ? source.size - at : 4;
            if (!copy_n(as_bytes(word), slice(source, at, length), length)) return false;
            if (!write32(offset, load_le<std::uint32_t>(word))) return false;
            at += length;
        }
        return true;
    }
private:
    bool valid(std::size_t offset, std::size_t width) const {
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        return base && offset <= size && width <= size - offset &&
               offset <= ~std::uintptr_t{0} - address && !((address + offset) & (width - 1));
    }
};

} // namespace libk

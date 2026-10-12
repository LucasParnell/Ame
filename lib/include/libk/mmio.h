#pragma once
// Bounded volatile register access. The caller owns the mapping/capability and
// supplies hardware-specific ordering; this view performs no hidden barriers.
#include <cstddef>
#include <cstdint>

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
private:
    bool valid(std::size_t offset, std::size_t width) const {
        const auto address = reinterpret_cast<std::uintptr_t>(base);
        return base && offset <= size && width <= size - offset &&
               offset <= ~std::uintptr_t{0} - address && !((address + offset) & (width - 1));
    }
};

} // namespace libk

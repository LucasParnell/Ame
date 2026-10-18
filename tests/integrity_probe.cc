#include <cassert>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include "libk/hash.h"
#include "midix/format/integrity.h"

static uint32_t reference(const uint8_t* data, size_t size, uint32_t polynomial) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (polynomial & (0u - (crc & 1u)));
    }
    return ~crc;
}

int main() {
    uint8_t data[8201];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i * 137 + i / 3);
    // Unaligned, empty, partial and multiple progress chunks, every table index.
    for (size_t size : {size_t{0}, size_t{1}, size_t{255}, size_t{4095}, size_t{4096}, size_t{8199}}) {
        const auto* bytes = data + 1;
        assert(libk::crc32_ieee(libk::as_bytes(bytes, size)) == reference(bytes, size, 0xedb88320));
        assert(integrity::crc32c(bytes, size) == reference(bytes, size, 0x82f63b78));
        const auto split = size / 2;
        auto crc = libk::crc32c_update(~0u, libk::as_bytes(bytes, split));
        crc = libk::crc32c_update(crc, libk::as_bytes(bytes + split, size - split));
        assert(~crc == integrity::crc32c(bytes, size));
        size_t polls = 0;
        integrity::Progress progress{[](void* p) { ++*static_cast<size_t*>(p); }, &polls};
        assert(integrity::crc32c(bytes, size, progress) == ~crc);
        assert(polls == (size + 4095) / 4096);
    }
    // An overflowing zero range still masks every byte after its lower bound.
    uint8_t masked[32];
    for (size_t i = 0; i < 32; ++i) masked[i] = i < 3 ? data[i] : 0;
    assert(integrity::crc32c_zero_field(data, 32, 3, std::numeric_limits<size_t>::max()) ==
           integrity::crc32c(masked, 32));
    assert(integrity::crc32c_zero_field(data, 32, 32, 99) == integrity::crc32c(data, 32));
    const auto* vector = reinterpret_cast<const uint8_t*>("123456789");
    assert(integrity::crc32c(vector, 9) == 0xe3069283);
}

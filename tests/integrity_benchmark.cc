#include <chrono>
#include <cstdio>
#include <cstdint>
#include "libk/hash.h"
#include "midix/format/integrity.h"

alignas(64) static uint8_t bytes[1024 * 1024];
static volatile uint32_t result;
template <class F> void measure(const char* name, size_t size, F operation) {
    constexpr unsigned iterations = 16;
    const auto started = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) result = operation(bytes, size);
    const auto ns = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("case=%s ns_per_byte=%.4f\n", name, ns / (iterations * size));
}
int main() {
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<uint8_t>(i * 137 + i / 3);
    measure("ieee_4k", 4096, [](const uint8_t* p, size_t n) { return libk::crc32_ieee(libk::as_bytes(p, n)); });
    measure("crc32c_1m", sizeof(bytes), [](const uint8_t* p, size_t n) { return integrity::crc32c(p, n); });
    measure("crc32c_zero_1m", sizeof(bytes), [](const uint8_t* p, size_t n) { return integrity::crc32c_zero_field(p, n, 64, 4); });
    measure("sha256_1m", sizeof(bytes), [](const uint8_t* p, size_t n) {
        uint8_t digest[32]; integrity::sha256(p, n, digest); return static_cast<uint32_t>(digest[0]);
    });
}

#pragma once
#include <cstdint>
namespace libk {
    inline void metric_inc(uint64_t& c) {
        __atomic_add_fetch(&c, 1ull, __ATOMIC_RELAXED);
    }
    inline uint64_t metric_load(const uint64_t& c) {
        return __atomic_load_n(&c, __ATOMIC_RELAXED);
    }
} // namespace libk

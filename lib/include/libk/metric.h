#pragma once
#include <cstdint>
namespace libk {
inline void metric_add(uint64_t& c, uint64_t amount) {
    __atomic_add_fetch(&c, amount, __ATOMIC_RELAXED);
}
inline void metric_inc(uint64_t& c) {
    metric_add(c, 1);
}
inline uint64_t metric_load(const uint64_t& c) {
    return __atomic_load_n(&c, __ATOMIC_RELAXED);
}
} // namespace libk

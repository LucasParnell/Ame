#pragma once
// Shared-memory accesses. Device registers use libk::Mmio instead.
// Keep compiler intrinsics at this boundary and ordering explicit at callers.
namespace libk::atomic {
template <typename T> inline T load_relaxed(const T* value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}
template <typename T> inline T load_acquire(const T* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}
template <typename T, typename V> inline void store_relaxed(T* target, V value) {
    __atomic_store_n(target, static_cast<T>(value), __ATOMIC_RELAXED);
}
template <typename T, typename V> inline void store_release(T* target, V value) {
    __atomic_store_n(target, static_cast<T>(value), __ATOMIC_RELEASE);
}
} // namespace libk::atomic

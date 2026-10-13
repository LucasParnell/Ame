#pragma once
// Bounded binary min-heap operations over caller-owned arrays. Key reads the
// ordering key; position(element, index) records each move. Callers own lifetime,
// membership, synchronization, and policy. Keys must stay stable during a call.
#include <cstdint>
#include <cstddef>

namespace libk::indexed_heap {
inline constexpr uint32_t no_index = ~uint32_t{0};

template<class T, size_t N, class Key, class Position>
uint32_t sift_up(T (&data)[N], uint32_t i, Key key, Position position) {
    if (i >= N) return no_index;
    while (i) {
        const uint32_t parent = (i - 1) / 2;
        if (key(data[parent]) <= key(data[i])) break;
        const T value = data[parent];
        data[parent] = data[i];
        data[i] = value;
        position(data[parent], parent);
        position(data[i], i);
        i = parent;
    }
    return i;
}

template<class T, size_t N, class Key, class Position>
void sift_down(T (&data)[N], uint32_t size, uint32_t i, Key key, Position position) {
    if (size > N) return;
    while (i < size) {
        const uint32_t left = 2 * i + 1;
        const uint32_t right = left + 1;
        uint32_t smallest = i;
        if (left < size && key(data[left]) < key(data[smallest])) smallest = left;
        if (right < size && key(data[right]) < key(data[smallest])) smallest = right;
        if (smallest == i) break;
        const T value = data[i];
        data[i] = data[smallest];
        data[smallest] = value;
        position(data[i], i);
        position(data[smallest], smallest);
        i = smallest;
    }
}

template<class T, size_t N, class Key, class Position>
bool push(T (&data)[N], uint32_t& size, T value, Key key, Position position) {
    if (size >= N) return false;
    data[size] = value;
    position(value, size);
    ++size;
    sift_up(data, size - 1, key, position);
    return true;
}

template<class T, size_t N, class Key, class Position>
bool remove_at(T (&data)[N], uint32_t& size, uint32_t i, Key key, Position position) {
    if (size > N || i >= size) return false;
    const T removed = data[i];
    --size;
    if (i < size) {
        data[i] = data[size];
        position(data[i], i);
        i = sift_up(data, i, key, position);
        sift_down(data, size, i, key, position);
    }
    data[size] = T{};
    position(removed, no_index);
    return true;
}
} // namespace libk::indexed_heap

#pragma once
// Bounded text construction over caller-owned storage. No allocation or owning
// string: data, capacity, length and truncation are the complete writer state.
#include "libk/bytes.h"
#include "libk/printf.h"
#include "libk/str.h"

namespace libk::str {

struct Buffer {
    char* data;
    std::size_t capacity;
    std::size_t length = 0;
    bool truncated = false;

    Buffer(char* storage, std::size_t size) : data(storage), capacity(storage ? size : 0) {
        if (capacity) data[0] = '\0';
    }
    template <std::size_t N>
    explicit Buffer(char (&storage)[N]) : Buffer(storage, N) {}

    std::size_t remaining() const {
        return length < capacity ? capacity - length - 1 : 0;
    }

    // Append a counted run (it need not contain a NUL). A short append keeps the
    // prefix that fits, terminates it, and sets the sticky truncation flag.
    bool append(const char* text, std::size_t count) {
        if (!text && count != 0) {
            truncated = true;
            return false;
        }
        const auto available = remaining();
        const auto copied = count < available ? count : available;
        if (copied) {
            (void)libk::copy_n(libk::as_bytes(data + length, available),
                               libk::as_bytes(text, count), copied);
        }
        length += copied;
        if (length < capacity) data[length] = '\0';
        if (copied != count) truncated = true;
        return copied == count;
    }
    bool append(char ch) { return append(&ch, 1); }

    // Null means empty. Scan only as far as needed to detect truncation.
    bool append(const char* text) {
        return append(text, length_n(text, remaining() + 1));
    }

    template <typename... Args>
    bool format(const char* pattern, Args... args) {
        const auto space = length < capacity ? capacity - length : 0;
        const int needed = libk::snprintf(space ? data + length : nullptr, space, pattern, args...);
        if (needed < 0) {
            truncated = true;
            return false;
        }
        const auto count = static_cast<std::size_t>(needed);
        const auto available = remaining();
        length += count < available ? count : available;
        if (count > available) truncated = true;
        return count <= available;
    }
};

} // namespace libk::str

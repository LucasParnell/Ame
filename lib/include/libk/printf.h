#pragma once
#include <cstddef>
#include <cstdarg>

namespace libk {

using PutcharFn = void(*)(char);

// Register the character sink for printf/putchar (early UART, later a log ring).
void set_putchar(PutcharFn fn);
void putchar(char c);

// Formatted output to the registered sink.
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char* fmt, va_list ap);

// Bounded formatting into a caller buffer: writes at most cap-1 bytes plus a
// terminating NUL, and returns the length the output would have had (snprintf
// semantics). This is the caller-facing formatter — services use it instead of
// hand-rolling number/text conversion.
int snprintf(char* dst, std::size_t cap, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char* dst, std::size_t cap, const char* fmt, va_list ap);

void puts(const char* s);

} // namespace libk

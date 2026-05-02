#pragma once
#include <cstdarg>

namespace libk {

using PutcharFn = void(*)(char);

// Register character-output callback (points at UART putchar or a ring buffer).
void set_putchar(PutcharFn fn);
void putchar(char c);

// Minimal printf — %c %s %d %u %x %X %p %zu %%. No floating point.
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char* fmt, va_list ap);

void puts(const char* s);

} // namespace libk

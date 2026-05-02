// lib/libk/printf.cc — minimal printf for early boot logging.
//
// Spec ref: §12 Phase 0. No floating point. Supports:
//   %c %s %d %i %u %x %X %p %zu %lu %llu %%
//   flags: '0' (zero pad), '-' (left align)
//   width: decimal integer
//   length: 'l', 'll', 'z'
//
// Output goes through a single PutcharFn registered by the kernel
// (early UART putchar in phase 0; later replaced by a log ring).

#include "libk/printf.h"
#include "libk/string.h"

namespace libk {

static PutcharFn s_putchar = nullptr;

void set_putchar(PutcharFn fn) { s_putchar = fn; }

static void emit(char c) {
    if (s_putchar) s_putchar(c);
}

void putchar(char c) {
    emit(c);
}

static int emit_str(const char* s, int width, bool left, char pad) {
    int len = 0;
    for (const char* p = s; *p; ++p) ++len;
    int padcount = width > len ? width - len : 0;
    int written = 0;
    if (!left) {
        for (int i = 0; i < padcount; ++i) { emit(pad); ++written; }
    }
    for (int i = 0; i < len; ++i) { emit(s[i]); ++written; }
    if (left) {
        for (int i = 0; i < padcount; ++i) { emit(' '); ++written; }
    }
    return written;
}

// Format an unsigned 64-bit value into buf (right-aligned, returns start ptr).
// buf must be at least 24 chars (room for 64-bit binary safety + sign).
static char* fmt_u64(unsigned long long v, unsigned base, bool upper, char* buf_end) {
    static const char* lower_digits = "0123456789abcdef";
    static const char* upper_digits = "0123456789ABCDEF";
    const char* digits = upper ? upper_digits : lower_digits;
    char* p = buf_end;
    *--p = '\0';
    if (v == 0) {
        *--p = '0';
        return p;
    }
    while (v) {
        *--p = digits[v % base];
        v /= base;
    }
    return p;
}

int vprintf(const char* fmt, va_list ap) {
    int written = 0;
    while (*fmt) {
        char c = *fmt++;
        if (c != '%') {
            emit(c); ++written; continue;
        }

        // Flags.
        bool left = false, zero = false;
        for (;;) {
            if (*fmt == '-')      { left = true; ++fmt; }
            else if (*fmt == '0') { zero = true; ++fmt; }
            else break;
        }

        // Width.
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }

        // Length modifier.
        int lenmod = 0; // 0=int, 1=long, 2=long long, 3=size_t
        if (*fmt == 'l') {
            ++fmt; lenmod = 1;
            if (*fmt == 'l') { ++fmt; lenmod = 2; }
        } else if (*fmt == 'z') {
            ++fmt; lenmod = 3;
        }

        char conv = *fmt;
        if (conv == '\0') break;
        ++fmt;

        char pad = (zero && !left) ? '0' : ' ';
        char numbuf[24];
        char* numbuf_end = numbuf + sizeof(numbuf);

        switch (conv) {
        case '%':
            emit('%'); ++written;
            break;
        case 'c': {
            char ch = static_cast<char>(va_arg(ap, int));
            char tmp[2] = {ch, '\0'};
            written += emit_str(tmp, width, left, ' ');
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            written += emit_str(s, width, left, ' ');
            break;
        }
        case 'd': case 'i': {
            long long sv;
            if      (lenmod >= 2) sv = va_arg(ap, long long);
            else if (lenmod == 1) sv = va_arg(ap, long);
            else                  sv = va_arg(ap, int);
            unsigned long long uv = (sv < 0) ? static_cast<unsigned long long>(-(sv + 1)) + 1
                                             : static_cast<unsigned long long>(sv);
            char* p = fmt_u64(uv, 10, false, numbuf_end);
            if (sv < 0) *--p = '-';
            written += emit_str(p, width, left, pad);
            break;
        }
        case 'u': {
            unsigned long long uv;
            if      (lenmod >= 2) uv = va_arg(ap, unsigned long long);
            else if (lenmod == 1) uv = va_arg(ap, unsigned long);
            else if (lenmod == 3) uv = va_arg(ap, size_t);
            else                  uv = va_arg(ap, unsigned int);
            char* p = fmt_u64(uv, 10, false, numbuf_end);
            written += emit_str(p, width, left, pad);
            break;
        }
        case 'x': case 'X': {
            unsigned long long uv;
            if      (lenmod >= 2) uv = va_arg(ap, unsigned long long);
            else if (lenmod == 1) uv = va_arg(ap, unsigned long);
            else if (lenmod == 3) uv = va_arg(ap, size_t);
            else                  uv = va_arg(ap, unsigned int);
            char* p = fmt_u64(uv, 16, conv == 'X', numbuf_end);
            written += emit_str(p, width, left, pad);
            break;
        }
        case 'p': {
            uintptr_t v = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
            char* p = fmt_u64(v, 16, false, numbuf_end);
            *--p = 'x'; *--p = '0';
            written += emit_str(p, width, left, ' ');
            break;
        }
        default:
            emit('%'); emit(conv); written += 2;
            break;
        }
    }
    return written;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

void puts(const char* s) {
    while (*s) emit(*s++);
}

} // namespace libk

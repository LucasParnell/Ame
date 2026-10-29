// lib/libk/printf.cc — minimal printf, one formatting core, two sinks.
//
// Spec ref: §12 Phase 0. No floating point. Supports:
//   %c %s %d %i %u %x %X %p %zu %lu %llu %%
//   flags: '0' (zero pad), '-' (left align); width: decimal; length: l/ll/z
//
// The core (vformat) writes characters to a sink. printf/vprintf target the
// registered PutcharFn; snprintf/vsnprintf target a bounded caller buffer. One
// engine, so the two can never drift.

#include "libk/printf.h"

#include <cstdint>

namespace libk {

namespace {

using Sink = void (*)(void* ctx, char c);

void putchar_sink(void* ctx, char c) {
    auto* fn = static_cast<PutcharFn*>(ctx);
    if (*fn) (*fn)(c);
}

// Counts every character (so the snprintf return value matches) but only stores
// what fits, keeping one byte for the terminator.
struct BufferSink {
    char* dst;
    std::size_t cap;
    std::size_t len;
};
void buffer_sink(void* ctx, char c) {
    auto* b = static_cast<BufferSink*>(ctx);
    if (b->cap != 0 && b->len + 1 < b->cap) b->dst[b->len] = c;
    ++b->len;
}

} // namespace

static PutcharFn s_putchar = nullptr;

void set_putchar(PutcharFn fn) { s_putchar = fn; }

void putchar(char c) {
    if (s_putchar) s_putchar(c);
}

static int emit_str(Sink sink, void* ctx, const char* s, int width, bool left, char pad) {
    int len = 0;
    for (const char* p = s; *p; ++p) ++len;
    const int padcount = width > len ? width - len : 0;
    int written = 0;
    if (!left) {
        for (int i = 0; i < padcount; ++i) { sink(ctx, pad); ++written; }
    }
    for (int i = 0; i < len; ++i) { sink(ctx, s[i]); ++written; }
    if (left) {
        for (int i = 0; i < padcount; ++i) { sink(ctx, ' '); ++written; }
    }
    return written;
}

// Format an unsigned 64-bit value into buf (right-aligned); returns start ptr.
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

static int vformat(Sink sink, void* ctx, const char* fmt, va_list ap) {
    int written = 0;
    while (*fmt) {
        char c = *fmt++;
        if (c != '%') {
            sink(ctx, c); ++written; continue;
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
            sink(ctx, '%'); ++written;
            break;
        case 'c': {
            char ch = static_cast<char>(va_arg(ap, int));
            char tmp[2] = {ch, '\0'};
            written += emit_str(sink, ctx, tmp, width, left, ' ');
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            written += emit_str(sink, ctx, s, width, left, ' ');
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
            written += emit_str(sink, ctx, p, width, left, pad);
            break;
        }
        case 'u': {
            unsigned long long uv;
            if      (lenmod >= 2) uv = va_arg(ap, unsigned long long);
            else if (lenmod == 1) uv = va_arg(ap, unsigned long);
            else if (lenmod == 3) uv = va_arg(ap, size_t);
            else                  uv = va_arg(ap, unsigned int);
            char* p = fmt_u64(uv, 10, false, numbuf_end);
            written += emit_str(sink, ctx, p, width, left, pad);
            break;
        }
        case 'x': case 'X': {
            unsigned long long uv;
            if      (lenmod >= 2) uv = va_arg(ap, unsigned long long);
            else if (lenmod == 1) uv = va_arg(ap, unsigned long);
            else if (lenmod == 3) uv = va_arg(ap, size_t);
            else                  uv = va_arg(ap, unsigned int);
            char* p = fmt_u64(uv, 16, conv == 'X', numbuf_end);
            written += emit_str(sink, ctx, p, width, left, pad);
            break;
        }
        case 'p': {
            uintptr_t v = reinterpret_cast<uintptr_t>(va_arg(ap, void*));
            char* p = fmt_u64(v, 16, false, numbuf_end);
            *--p = 'x'; *--p = '0';
            written += emit_str(sink, ctx, p, width, left, ' ');
            break;
        }
        default:
            sink(ctx, '%'); sink(ctx, conv); written += 2;
            break;
        }
    }
    return written;
}

int vprintf(const char* fmt, va_list ap) {
    PutcharFn fn = s_putchar;
    return vformat(putchar_sink, &fn, fmt, ap);
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char* dst, std::size_t cap, const char* fmt, va_list ap) {
    BufferSink b{dst, cap, 0};
    const int would = vformat(buffer_sink, &b, fmt, ap);
    if (cap != 0) dst[b.len < cap ? b.len : cap - 1] = '\0';
    return would;
}

int snprintf(char* dst, std::size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return r;
}

void puts(const char* s) {
    while (*s) putchar(*s++);
}

} // namespace libk

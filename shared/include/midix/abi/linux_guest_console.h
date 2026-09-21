#pragma once
/*
 * linux_guest_console.h — freestanding console/string helpers for the dango
 * Linux appliance userspace. Shared by the agent and the kms self-test so the
 * strlen/puts/append primitive exists exactly once.
 *
 * C-only (see linux_guest_syscall.h): the trampolines it builds on are C-only.
 */
#ifndef __cplusplus

#ifndef MIDIX_ABI_LINUX_GUEST_CONSOLE_H
#define MIDIX_ABI_LINUX_GUEST_CONSOLE_H

#include "midix/abi/linux_guest_syscall.h"

static inline unsigned long mx_strlen(const char* s) {
    unsigned long n = 0;
    while (s[n]) ++n;
    return n;
}

/* Returns 1 when the first n bytes match, 0 otherwise. */
static inline int mx_memeq(const void* a, const void* b, unsigned long n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (unsigned long i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static inline void mx_zero(void* dst, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; ++i) d[i] = 0;
}

static inline void mx_puts(const char* s) {
    (void)mx_write(1, s, mx_strlen(s));
}

/* Append an unsigned decimal to `out`; returns the new end pointer. */
static inline char* mx_append_u32(char* out, unsigned int v) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        *out++ = '0';
        return out;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n) *out++ = tmp[--n];
    return out;
}

static inline char* mx_append_str(char* out, const char* s) {
    while (*s) *out++ = *s++;
    return out;
}

#endif /* MIDIX_ABI_LINUX_GUEST_CONSOLE_H */

#else /* __cplusplus */
#error "linux_guest_console.h is C-only"
#endif /* __cplusplus */

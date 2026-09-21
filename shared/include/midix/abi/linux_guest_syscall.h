#pragma once
/*
 * linux_guest_syscall.h — freestanding aarch64 Linux syscall trampolines.
 *
 * Single shared copy of the raw `svc #0` ABI used by every dango appliance
 * userspace translation unit (agent + channel_test + kms_test). The register
 * mapping is Linux aarch64: x8 = syscall number, x0..x5 = args, x0 = result.
 *
 * C-only: the `register ... __asm__("xN")` extended-asm form is a C contract
 * and is not expressible in C++; a C++ inclusion is a deliberate hard error
 * rather than a silently different ABI.
 */
#ifndef __cplusplus

#ifndef MIDIX_ABI_LINUX_GUEST_SYSCALL_H
#define MIDIX_ABI_LINUX_GUEST_SYSCALL_H

/* Syscall numbers (asm-generic/unistd.h, aarch64). */
#define MX_NR_DUP3 24
#define MX_NR_IOCTL 29
#define MX_NR_MOUNT 40
#define MX_NR_OPENAT 56
#define MX_NR_CLOSE 57
#define MX_NR_READ 63
#define MX_NR_WRITE 64
#define MX_NR_PPOLL 73
#define MX_NR_NANOSLEEP 101
#define MX_NR_MUNMAP 215
#define MX_NR_MMAP 222

/* openat(2) dirfd + flags, mmap(2) prot/flags. */
#define MX_AT_FDCWD (-100)
#define MX_O_RDONLY 0
#define MX_O_RDWR 2
#define MX_PROT_READ 0x1
#define MX_PROT_WRITE 0x2
#define MX_PROT_EXEC 0x4
#define MX_MAP_SHARED 0x1

/* Errnos the channel device deliberately returns (linux/errno.h). */
#define MX_EBUSY 16
#define MX_EINVAL 22
#define MX_ENOTTY 25

/* ppoll(2) sigsetsize for aarch64 (sizeof(struct kernel_sigset_t)). */
#define MX_SIGSET_BYTES 8

struct mx_timespec {
    long tv_sec;
    long tv_nsec;
};

struct mx_pollfd {
    int fd;
    short events;
    short revents;
};

/* Raw six-argument trampoline; every wrapper below funnels through it. */
static inline long mx_syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
}

static inline long mx_openat(int dirfd, const char* path, int flags, unsigned long mode) {
    return mx_syscall6(MX_NR_OPENAT, dirfd, (long)path, flags, (long)mode, 0, 0);
}

static inline long mx_close(int fd) {
    return mx_syscall6(MX_NR_CLOSE, fd, 0, 0, 0, 0, 0);
}

static inline long mx_read(int fd, void* buf, unsigned long len) {
    return mx_syscall6(MX_NR_READ, fd, (long)buf, (long)len, 0, 0, 0);
}

static inline long mx_write(int fd, const void* buf, unsigned long len) {
    return mx_syscall6(MX_NR_WRITE, fd, (long)buf, (long)len, 0, 0, 0);
}

static inline long mx_ioctl(int fd, unsigned long request, void* arg) {
    return mx_syscall6(MX_NR_IOCTL, fd, (long)request, (long)arg, 0, 0, 0);
}

static inline long mx_mmap(void* addr, unsigned long len, int prot, int flags, int fd, long offset) {
    return mx_syscall6(MX_NR_MMAP, (long)addr, (long)len, prot, flags, fd, offset);
}

static inline long mx_munmap(void* addr, unsigned long len) {
    return mx_syscall6(MX_NR_MUNMAP, (long)addr, (long)len, 0, 0, 0, 0);
}

static inline long mx_dup3(int oldfd, int newfd, int flags) {
    return mx_syscall6(MX_NR_DUP3, oldfd, newfd, flags, 0, 0, 0);
}

static inline long mx_nanosleep(const struct mx_timespec* req, struct mx_timespec* rem) {
    return mx_syscall6(MX_NR_NANOSLEEP, (long)req, (long)rem, 0, 0, 0, 0);
}

static inline long mx_ppoll(struct mx_pollfd* fds, int nfds, const struct mx_timespec* timeout) {
    return mx_syscall6(MX_NR_PPOLL, (long)fds, nfds, (long)timeout, 0, MX_SIGSET_BYTES, 0);
}

static inline long mx_mount(const char* source, const char* target, const char* fstype) {
    return mx_syscall6(MX_NR_MOUNT, (long)source, (long)target, (long)fstype, 0, 0, 0);
}

#endif /* MIDIX_ABI_LINUX_GUEST_SYSCALL_H */

#else /* __cplusplus */
#error "linux_guest_syscall.h is C-only"
#endif /* __cplusplus */

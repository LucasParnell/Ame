#include <stddef.h>
#include <stdint.h>

#define AT_FDCWD (-100)
#define O_RDWR 00000002

static long sys_dup3(int oldfd, int newfd, int flags) {
    register long x0 asm("x0") = oldfd;
    register long x1 asm("x1") = newfd;
    register long x2 asm("x2") = flags;
    register long x8 asm("x8") = 24;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_openat(int dirfd, const char* path, int flags, unsigned long mode) {
    register long x0 asm("x0") = dirfd;
    register const char* x1 asm("x1") = path;
    register long x2 asm("x2") = flags;
    register unsigned long x3 asm("x3") = mode;
    register long x8 asm("x8") = 56;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_close(int fd) {
    register long x0 asm("x0") = fd;
    register long x8 asm("x8") = 57;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8)
                 : "memory");
    return x0;
}

static long sys_write(int fd, const void* buf, unsigned long len) {
    register long x0 asm("x0") = fd;
    register const void* x1 asm("x1") = buf;
    register unsigned long x2 asm("x2") = len;
    register long x8 asm("x8") = 64;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_nanosleep(const void* req, void* rem) {
    register long x0 asm("x0") = (long)req;
    register void* x1 asm("x1") = rem;
    register long x8 asm("x8") = 101;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x8)
                 : "memory");
    return x0;
}

struct timespec_compat {
    long tv_sec;
    long tv_nsec;
};

static void ensure_stdio_console(void) {
    long fd = sys_openat(AT_FDCWD, "/dev/console", O_RDWR, 0);
    if (fd < 0) {
        return;
    }

    for (int target = 0; target < 3; ++target) {
        if (fd != target) {
            (void)sys_dup3((int)fd, target, 0);
        }
    }

    if (fd > 2) {
        (void)sys_close((int)fd);
    }
}

#ifdef DANGO_GPU_PROBE
// Linux aarch64 UAPI. The probe uses only mount/read/ioctl; no DRM or virtio
// implementation is duplicated in the host or in this appliance userspace.
static long call6(long nr, long a, long b, long c, long d, long e, long f) {
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    register long x3 asm("x3") = d;
    register long x4 asm("x4") = e;
    register long x5 asm("x5") = f;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4),
                 "r"(x5), "r"(x8) : "memory");
    return x0;
}
static int mount_fs(const char* name, const char* target) {
    return call6(40, (long)name, (long)target, (long)name, 0, 0, 0) == 0;
}
static int equal(const char* a, const char* b, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static void message(const char* msg) {
    unsigned long n = 0;
    while (msg[n]) ++n;
    (void)sys_write(1, msg, n);
}
static int gpu_probe(void) {
    if (!mount_fs("devtmpfs", "/dev") || !mount_fs("proc", "/proc") ||
        !mount_fs("sysfs", "/sys")) return 0;
    long fd = sys_openat(AT_FDCWD, "/dev/dri/card0", O_RDWR, 0);
    if (fd < 0) return 0;
    char name[32] = {0};
    struct drm_version {
        int major, minor, patch;
        unsigned long name_len;
        char* name;
        unsigned long date_len;
        char* date;
        unsigned long desc_len;
        char* desc;
    } version = {0, 0, 0, sizeof(name), name, 0, 0, 0, 0};
    _Static_assert(sizeof(version) == 64, "aarch64 DRM_VERSION layout");
    const unsigned long version_ioctl = 0xc0406400;
    long rc = call6(29, fd, version_ioctl, (long)&version, 0, 0, 0);
    struct { uint64_t capability, value; } cap = {1, 0};
    long cap_rc = call6(29, fd, 0xc010640c, (long)&cap, 0, 0, 0);
    (void)sys_close(fd);
    if (rc || version.name_len != 10 || !equal(name, "virtio_gpu", 10) || cap_rc || cap.value != 1)
        return 0;
    message("dango: DRM card0 driver=virtio_gpu dumb_buffer=1\n");

    // The kernel's boot-time virtqueue requests require real completions.
    // Independently check Linux accounted at least one virtio ISR invocation.
    fd = sys_openat(AT_FDCWD, "/proc/interrupts", 0, 0);
    if (fd < 0) return 0;
    char interrupts[4096];
    long n = call6(63, fd, (long)interrupts, sizeof(interrupts), 0, 0, 0);
    (void)sys_close(fd);
    if (n <= 0 || n >= (long)sizeof(interrupts)) return 0;
    for (long begin = 0; begin < n;) {
        long end = begin;
        while (end < n && interrupts[end] != '\n') ++end;
        int virtio = 0;
        for (long i = begin; i + 6 <= end; ++i)
            if (equal(interrupts + i, "virtio", 6)) virtio = 1;
        long pos = begin;
        while (pos < end && interrupts[pos] != ':') ++pos;
        if (pos < end) ++pos;
        while (pos < end && interrupts[pos] == ' ') ++pos;
        unsigned long count = 0;
        while (pos < end && interrupts[pos] >= '0' && interrupts[pos] <= '9')
            count = count * 10 + (interrupts[pos++] - '0');
        if (virtio && count) {
            message("dango: Linux virtio ISR observed\n");
            return 1;
        }
        begin = end + 1;
    }
    return 0;
}
#endif

void _start(void) {
    ensure_stdio_console();

    const char msg[] =
        "dango-agent: started\n"
        "dango: linux guest responded\n";

    if (sys_write(1, msg, sizeof(msg) - 1) < 0) {
        (void)sys_write(2, msg, sizeof(msg) - 1);
    }

#ifdef DANGO_GPU_PROBE
    message(gpu_probe() ? "dango: gpu probe passed\n" : "dango: gpu probe FAILED\n");
#endif

    for (;;) {
        struct timespec_compat ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        sys_nanosleep(&ts, 0);
    }
}

/* Freestanding runtime shims. The agent links with -nostdlib -fno-builtin, but
 * a compiler may still emit calls to these for block copies/clears: clang
 * usually inlines them, gcc does not. Keep them trivial and dependency-free so
 * the same source links under both. */
void* memcpy(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* memset(void* dst, int c, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; ++i) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

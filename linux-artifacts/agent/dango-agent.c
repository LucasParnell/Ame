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

void _start(void) {
    ensure_stdio_console();

    const char msg[] =
        "dango-agent: started\n"
        "dango: linux guest responded\n";

    if (sys_write(1, msg, sizeof(msg) - 1) < 0) {
        (void)sys_write(2, msg, sizeof(msg) - 1);
    }

    for (;;) {
        struct timespec_compat ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        sys_nanosleep(&ts, 0);
    }
}

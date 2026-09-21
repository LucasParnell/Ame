#include <stddef.h>
#include <stdint.h>

#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/linux_drm_uapi.h"
#include "midix/abi/linux_guest_console.h"
#include "midix/abi/linux_guest_syscall.h"

static void ensure_stdio_console(void) {
    long fd = mx_openat(MX_AT_FDCWD, DANGO_DEV_CONSOLE, MX_O_RDWR, 0);
    if (fd < 0) {
        return;
    }

    for (int target = 0; target < 3; ++target) {
        if (fd != target) {
            (void)mx_dup3((int)fd, target, 0);
        }
    }

    if (fd > 2) {
        (void)mx_close((int)fd);
    }
}

#ifdef DANGO_GPU_KMS
extern int dango_kms_test(void);
#endif

#ifdef DANGO_GPU_PROBE
// Linux aarch64 UAPI. The probe uses only mount/read/ioctl; no DRM or virtio
// implementation is duplicated in the host or in this appliance userspace.
static const char k_virtio_isr_tag[] = "virtio";

static int mount_fs(const char* name, const char* target) {
    return mx_mount(name, target, name) == 0;
}

static int gpu_probe(void) {
    if (!mount_fs("devtmpfs", "/dev") || !mount_fs("proc", "/proc") ||
        !mount_fs("sysfs", "/sys")) return 0;
    long fd = mx_openat(MX_AT_FDCWD, DANGO_DEV_DRM_CARD0, MX_O_RDWR, 0);
    if (fd < 0) return 0;
    char name[32] = {0};
    struct drm_version version = {0, 0, 0, sizeof(name), name, 0, 0, 0, 0};
    long rc = mx_ioctl((int)fd, DRM_IOCTL_VERSION, &version);
    struct drm_get_cap cap = {1, 0};
    long cap_rc = mx_ioctl((int)fd, DRM_IOCTL_GET_CAP, &cap);
    (void)mx_close((int)fd);
    if (rc || version.name_len != DANGO_GPU_DRIVER_LEN ||
        !mx_memeq(name, DANGO_GPU_DRIVER, DANGO_GPU_DRIVER_LEN) || cap_rc || cap.value != 1)
        return 0;
    mx_puts("dango: DRM card0 driver=" DANGO_GPU_DRIVER " dumb_buffer=1\n");

    // The kernel's boot-time virtqueue requests require real completions.
    // Independently check Linux accounted at least one virtio ISR invocation.
    fd = mx_openat(MX_AT_FDCWD, DANGO_DEV_PROC_INTERRUPTS, MX_O_RDONLY, 0);
    if (fd < 0) return 0;
    char interrupts[4096];
    long n = mx_read((int)fd, interrupts, sizeof(interrupts));
    (void)mx_close((int)fd);
    // A short or buffer-filling read is not an error: scan exactly the bytes
    // returned, line by line. Only a negative read result is a failure.
    if (n < 0) return 0;
    const long tag_len = (long)(sizeof(k_virtio_isr_tag) - 1);
    for (long begin = 0; begin < n;) {
        long end = begin;
        while (end < n && interrupts[end] != '\n') ++end;
        int virtio = 0;
        for (long i = begin; i + tag_len <= end; ++i)
            if (mx_memeq(interrupts + i, k_virtio_isr_tag, (unsigned long)tag_len)) virtio = 1;
        long pos = begin;
        while (pos < end && interrupts[pos] != ':') ++pos;
        if (pos < end) ++pos;
        while (pos < end && interrupts[pos] == ' ') ++pos;
        unsigned long count = 0;
        while (pos < end && interrupts[pos] >= '0' && interrupts[pos] <= '9')
            count = count * 10 + (unsigned long)(interrupts[pos++] - '0');
        if (virtio && count) {
            mx_puts("dango: Linux virtio ISR observed\n");
            return 1;
        }
        begin = end + 1;
    }
    return 0;
}
#endif

#ifdef DANGO_CHANNEL_TEST
int dango_channel_test(void);
#endif

#ifdef DANGO_GPU_GPUD
int dango_gpud_run(void);
#endif

void _start(void) {
    ensure_stdio_console();

    const char msg[] =
        "dango-agent: started\n"
        "dango: linux guest responded\n";

    if (mx_write(1, msg, sizeof(msg) - 1) < 0) {
        (void)mx_write(2, msg, sizeof(msg) - 1);
    }

#ifdef DANGO_GPU_PROBE
    if (gpu_probe()) {
        mx_puts("dango: gpu probe passed\n");
#ifdef DANGO_GPU_GPUD
        // Phase H/I: dango-gpud serves the Midix GPU protocol over the Dango
        // channel and drives the staging present path. It runs *before* the KMS
        // self-test because only the first opener of the primary node becomes
        // DRM master and can issue SETCRTC (drm_master_open()/drm_setmaster_ioctl
        // reject a second master with -EBUSY), which both users need. Once the
        // host's bootstrap session is done gpud drops master, keeps the card
        // open so the presented frame stays on screen, closes the channel and
        // returns, leaving the KMS and transport self-tests below untouched.
        (void)dango_gpud_run();
        mx_puts("dango-gpud: exited\n");
#endif
#ifdef DANGO_GPU_KMS
        // Phase F: drive a real KMS modeset so the host can assert the
        // displayed pattern (scripts/test/kms-display.py). Raw DRM ioctls live
        // in linux-artifacts/kms_test/kms_test.c.
        if (dango_kms_test()) {
            mx_puts("dango: kms test passed\n");
        }
#endif
    } else {
        mx_puts("dango: gpu probe FAILED\n");
    }
#endif

#ifdef DANGO_CHANNEL_TEST
    mx_puts(dango_channel_test() ? "dango: channel test passed\n" : "dango: channel test FAILED\n");
#endif

    for (;;) {
        struct mx_timespec ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        (void)mx_nanosleep(&ts, 0);
    }
}

/* Freestanding runtime shims. The agent links with -nostdlib -fno-builtin, but
 * a compiler may still emit calls to these for block copies/clears: clang
 * usually inlines them, gcc does not. Keep them trivial and dependency-free so
 * the same source links under both. These are global (non-static) by cross-TU
 * link contract: channel_test.c/kms_test.c struct assignments lower to them. */
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

// Read-only Linux AArch64 framebuffer inspection, with no Android libc.
// Build with LLVM clang --target=aarch64-linux-gnu -fuse-ld=lld -nostdlib
// -static -ffreestanding -fno-stack-protector -Wl,-e,_start.
typedef unsigned int u32;
typedef unsigned long u64;
typedef long s64;

struct bitfield { u32 offset, length, msb_right; };
struct fixed {
    char id[16];
    u64 smem_start;
    u32 smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    u32 line_length;
    u64 mmio_start;
    u32 mmio_len, accel;
    unsigned short capabilities, reserved[2];
};
struct variable {
    u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    u32 bits_per_pixel, grayscale;
    struct bitfield red, green, blue, transp;
    u32 nonstd, activate, height, width, accel_flags, pixclock;
    u32 left_margin, right_margin, upper_margin, lower_margin;
    u32 hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    u32 reserved[4];
};
struct report {
    char magic[4];
    u32 version;
    s64 open_result, fixed_result, variable_result;
    struct fixed fixed;
    struct variable variable;
};
_Static_assert(sizeof(struct fixed) == 80, "Linux AArch64 fixed ABI");
_Static_assert(sizeof(struct variable) == 160, "Linux variable ABI");
_Static_assert(sizeof(struct report) == 272, "probe report ABI");
static struct report report = {{'F','B','Q','1'}, 1, -1, -1, -1, {0}, {0}};

static s64 syscall4(s64 nr, s64 a, s64 b, s64 c, s64 d) {
    register s64 x0 __asm__("x0") = a;
    register s64 x1 __asm__("x1") = b;
    register s64 x2 __asm__("x2") = c;
    register s64 x3 __asm__("x3") = d;
    register s64 x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}
__attribute__((noreturn)) void _start(void) {
    // openat(O_RDONLY | O_CLOEXEC), FBIOGET_FSCREENINFO, FBIOGET_VSCREENINFO.
    report.open_result = syscall4(56, -100, (s64)"/dev/graphics/fb0", 0x80000, 0);
    if (report.open_result >= 0) {
        report.fixed_result = syscall4(29, report.open_result, 0x4602, (s64)&report.fixed, 0);
        report.variable_result = syscall4(29, report.open_result, 0x4600, (s64)&report.variable, 0);
        syscall4(57, report.open_result, 0, 0, 0);
    }
    u64 done = 0;
    while (done < sizeof(report)) {
        s64 written = syscall4(64, 1, (s64)&report + done, sizeof(report) - done, 0);
        if (written <= 0) break;
        done += written;
    }
    syscall4(94, report.open_result < 0 || report.fixed_result || report.variable_result || done != sizeof(report), 0, 0, 0);
    __builtin_unreachable();
}

/* Phase G transport acceptance. No UART carries requests or replies. */
#include <stdint.h>
#include "midix/abi/dango_channel_guest.h"

struct ring {
    uint64_t head;
    uint8_t pad0[56];
    uint64_t tail;
    uint8_t pad1[184];
};
struct slot { uint32_t type, length; uint64_t seq; uint8_t payload[48]; };
_Static_assert(sizeof(struct ring) == 256, "ring header ABI");
_Static_assert(sizeof(struct slot) == 64, "ring slot ABI");

static long call(long nr, long a, long b, long c, long d, long e, long f)
{
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
    register long x3 asm("x3") = d, x4 asm("x4") = e, x5 asm("x5") = f;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}

static int send(int fd, struct ring *tx, uint32_t type, uint64_t value, uint32_t len)
{
    uint64_t head = __atomic_load_n(&tx->head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE);
    struct slot *slot = (struct slot *)(tx + 1) + (head & 63);
    if (head - tail >= 64) return 0;
    *slot = (struct slot){ .type = type, .length = len, .seq = head };
    for (unsigned i = 0; i < len; ++i) slot->payload[i] = (uint8_t)(value >> (i * 8));
    __atomic_store_n(&tx->head, head + 1, __ATOMIC_RELEASE);
    return call(29, fd, DANGO_CH_NOTIFY, 0, 0, 0, 0) == 0;
}

int dango_channel_test(void)
{
    struct dango_channel_layout layout;
    long fd = call(56, -100, (long)"/dev/dango-channel0", 2, 0, 0, 0);
    long mapping;
    struct ring *rx, *tx;
    uint64_t interrupts = 0, start;
    if (fd < 0) return 0;
    if (call(29, fd, DANGO_CH_QUERY, (long)&layout, 0, 0, 0) ||
        layout.desc.magic != DANGO_CH_MAGIC || layout.desc.version_major != 1 ||
        layout.desc.ring_order != 6 || layout.ring_bytes != 8192 || layout.mapping_bytes != 16384)
        return 0;
    /* Negative ABI cases: no arbitrary offsets, executable aliases or ioctl. */
    if (call(56, -100, (long)"/dev/dango-channel0", 2, 0, 0, 0) != -16 ||
        call(29, fd, DANGO_CH_NOTIFY, 1, 0, 0, 0) != -22 ||
        call(29, fd, 0xffffffffU, 0, 0, 0, 0) != -25 ||
        call(222, 0, layout.mapping_bytes, 3, 1, fd, 4096) != -22 ||
        call(222, 0, layout.mapping_bytes, 7, 1, fd, 0) != -22)
        return 0;
    mapping = call(222, 0, layout.mapping_bytes, 3, 1, fd, 0);
    if (mapping < 0) return 0;
    rx = (struct ring *)mapping;
    tx = (struct ring *)(mapping + layout.ring_bytes);
    start = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
    if (!send(fd, tx, 0x47430001u, 0, 0)) return 0;
    for (uint64_t n = 0; n < 512; ++n) {
        struct slot message;
        uint64_t tail, head, value = 0;
        if (n & 1) {
            struct { int fd; short events, revents; } pollfd = { (int)fd, 1, 0 };
            struct { long seconds, nanoseconds; } timeout = { 5, 0 };
            if (call(73, (long)&pollfd, 1, (long)&timeout, 0, 8, 0) != 1 ||
                !(pollfd.revents & 1)) return 0;
        }
        if (call(63, fd, (long)&interrupts, sizeof(interrupts), 0, 0, 0) != sizeof(interrupts)) return 0;
        tail = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
        head = __atomic_load_n(&rx->head, __ATOMIC_ACQUIRE);
        if (head - tail != 1) return 0;
        message = *((struct slot *)(rx + 1) + (tail & 63));
        if (message.type != 0x47430002u || message.length != 8 || message.seq != start + n) return 0;
        for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)message.payload[i] << (i * 8);
        if (value != n) return 0;
        __atomic_store_n(&rx->tail, tail + 1, __ATOMIC_RELEASE);
        if (!send(fd, tx, 0x47430003u, n, 8)) return 0;
    }
    if (!interrupts || !send(fd, tx, 0x47430004u, interrupts, 8)) return 0;
    call(215, mapping, layout.mapping_bytes, 0, 0, 0, 0);
    call(57, fd, 0, 0, 0, 0, 0);
    return 1;
}

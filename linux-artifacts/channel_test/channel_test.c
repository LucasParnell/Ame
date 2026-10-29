/* Phase G transport acceptance. No UART carries requests or replies.
 *
 * Relies on the agent's global memcpy/memset (cross-TU link contract): the
 * struct assignments below lower to memcpy(), provided by linux-artifacts/agent.
 */
#include <stdint.h>

#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/dango_channel_test.h"
#include "midix/abi/linux_guest_syscall.h"

/* Slot index mask and ring capacity, both derived from the ring order. */
#define DANGO_CH_RING_MASK (DANGO_RING_SLOTS(DANGO_CH_DEFAULT_RING_ORDER) - 1U)

static int send(int fd, struct dango_ring_hdr *tx, uint32_t type, uint64_t value, uint32_t len)
{
    uint64_t head = __atomic_load_n(&tx->head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE);
    struct dango_ring_slot *slot =
        (struct dango_ring_slot *)(tx + 1) + (head & DANGO_CH_RING_MASK);
    if (head - tail >= DANGO_RING_SLOTS(DANGO_CH_DEFAULT_RING_ORDER)) return 0;
    *slot = (struct dango_ring_slot){ .type = type, .length = len, .seq = head };
    for (uint32_t i = 0; i < len; ++i) slot->payload[i] = (uint8_t)(value >> (i * 8));
    __atomic_store_n(&tx->head, head + 1, __ATOMIC_RELEASE);
    return mx_ioctl(fd, DANGO_CH_NOTIFY, 0) == 0;
}

int dango_channel_test(void)
{
    struct dango_channel_layout layout;
    long fd = mx_openat(MX_AT_FDCWD, DANGO_DEV_CHANNEL, MX_O_RDWR, 0);
    long mapping;
    struct dango_ring_hdr *rx, *tx;
    uint64_t interrupts = 0, start;
    const uint32_t value_bytes = (uint32_t)sizeof(uint64_t);
    if (fd < 0) return 0;
    if (mx_ioctl((int)fd, DANGO_CH_QUERY, &layout) ||
        layout.desc.magic != DANGO_CH_MAGIC || layout.desc.version_major != DANGO_CH_VERSION ||
        layout.desc.ring_order != DANGO_CH_DEFAULT_RING_ORDER ||
        layout.ring_bytes != DANGO_RING_BYTES(DANGO_CH_DEFAULT_RING_ORDER) ||
        layout.mapping_bytes != DANGO_RING_MAPPING_BYTES(DANGO_CH_DEFAULT_RING_ORDER))
        return 0;
    /* Negative ABI cases: no arbitrary offsets, executable aliases or ioctl. */
    if (mx_openat(MX_AT_FDCWD, DANGO_DEV_CHANNEL, MX_O_RDWR, 0) != -MX_EBUSY ||
        mx_ioctl((int)fd, DANGO_CH_NOTIFY, (void *)1) != -MX_EINVAL ||
        mx_ioctl((int)fd, 0xffffffffU, 0) != -MX_ENOTTY ||
        mx_mmap(0, layout.mapping_bytes, MX_PROT_READ | MX_PROT_WRITE, MX_MAP_SHARED, (int)fd, 4096) != -MX_EINVAL ||
        mx_mmap(0, layout.mapping_bytes, MX_PROT_READ | MX_PROT_WRITE | MX_PROT_EXEC, MX_MAP_SHARED, (int)fd, 0) != -MX_EINVAL)
        return 0;
    mapping = mx_mmap(0, layout.mapping_bytes, MX_PROT_READ | MX_PROT_WRITE, MX_MAP_SHARED, (int)fd, 0);
    if (mapping < 0) return 0;
    rx = (struct dango_ring_hdr *)mapping;
    tx = (struct dango_ring_hdr *)(mapping + layout.ring_bytes);
    start = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
    if (!send(fd, tx, DANGO_CH_TEST_READY, 0, 0)) return 0;
    for (uint64_t n = 0; n < DANGO_CH_TEST_ROUNDS; ++n) {
        struct dango_ring_slot message;
        uint64_t tail, head, value = 0;
        if (n & 1) {
            struct mx_pollfd pollfd = { (int)fd, 1, 0 };
            struct mx_timespec timeout = { 5, 0 };
            if (mx_ppoll(&pollfd, 1, &timeout) != 1 || !(pollfd.revents & 1)) return 0;
        }
        if (mx_read((int)fd, &interrupts, sizeof(interrupts)) != (long)sizeof(interrupts)) return 0;
        tail = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
        head = __atomic_load_n(&rx->head, __ATOMIC_ACQUIRE);
        if (head - tail != 1) return 0;
        message = *((struct dango_ring_slot *)(rx + 1) + (tail & DANGO_CH_RING_MASK));
        if (message.type != DANGO_CH_TEST_PING || message.length != value_bytes ||
            message.seq != start + n)
            return 0;
        for (uint32_t i = 0; i < value_bytes; ++i) value |= (uint64_t)message.payload[i] << (i * 8);
        if (value != n) return 0;
        __atomic_store_n(&rx->tail, tail + 1, __ATOMIC_RELEASE);
        if (!send(fd, tx, DANGO_CH_TEST_PONG, n, value_bytes)) return 0;
    }
    if (!interrupts || !send(fd, tx, DANGO_CH_TEST_DONE, interrupts, value_bytes)) return 0;
    (void)mx_munmap((void *)mapping, layout.mapping_bytes);
    (void)mx_close((int)fd);
    return 1;
}

// userland/drivers/blk/virtio/main.cc — drv.blk.virtio service.
//
// V1 block backend for io.fs. Uses virtio-mmio modern or legacy queues and
// polls completions, matching the rest of the current driver bring-up.

#include "mochi/syscall.h"
#include "mochi/arena.h"
#include "libk/bytes.h"
#include "libk/integer.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/fs.h"
#include "midix/abi/protocols/fs.h"
#include "mochi/va_layout.h"
#include "mochi/wait.h"
#include "mochi/driver.h"
#include "libk/cpu.h"

using namespace mochi::va::blk;

constexpr mx_u32_t kVirtioMmioSlotSize = 0x200;
constexpr mx_u32_t kVirtioMmioScanSize = 0x4000;
constexpr mx_u16_t kQueueSize = 8;
constexpr mx_u32_t kBlkSectorSize = 512;
constexpr mx_u32_t kVirtioWaitSpins = 1'000'000;

static mx_u32_t g_virtio_slot_offset = 0;
static volatile mx_u8_t* g_mmio = nullptr;
static mx_u8_t* g_dma_base = nullptr;
static mx_u64_t g_dma_phys = 0;
static mx_u64_t g_dma_size = 0;
static mochi::Arena g_dma_arena;
static mx_u64_t g_data_region_offset = 0;
static mx_u64_t g_capacity_sectors = 0;
static mx_u16_t g_avail_idx = 0;
static mx_u16_t g_used_idx = 0;
static bool g_blk_ready = false;
static bool g_flush_supported = false;
static bool g_legacy_transport = false;

// Virtio-MMIO register offsets. These are intentionally present in the V0
// skeleton so the real queue setup lands without changing the service shape.
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_MAGIC = 0x000;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_VERSION = 0x004;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_DEVICE_ID = 0x008;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_HOST_FEATURES = 0x010;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_HOST_FEATURES_SEL = 0x014;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_GUEST_FEATURES = 0x020;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_GUEST_FEATURES_SEL = 0x024;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_GUEST_PAGE_SIZE = 0x028;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_SEL = 0x030;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_NUM = 0x038;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_ALIGN = 0x03C;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_PFN = 0x040;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_NOTIFY = 0x050;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_INTERRUPT_STATUS = 0x060;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_INTERRUPT_ACK = 0x064;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_STATUS = 0x070;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_READY = 0x044;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_DESC_HIGH = 0x084;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0A0;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0A4;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_CONFIG_GENERATION = 0x0FC;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_MMIO_CONFIG = 0x100;

[[maybe_unused]] constexpr mx_u32_t VIRTIO_BLK_F_BLK_SIZE = 1u << 6;
[[maybe_unused]] constexpr mx_u32_t VIRTIO_BLK_F_FLUSH = 1u << 9;
constexpr mx_u32_t VIRTIO_F_VERSION_1_BIT = 0; // feature bit 32 in selector 1

constexpr mx_u32_t VIRTIO_STATUS_ACKNOWLEDGE = 1u;
constexpr mx_u32_t VIRTIO_STATUS_DRIVER = 2u;
constexpr mx_u32_t VIRTIO_STATUS_DRIVER_OK = 4u;
constexpr mx_u32_t VIRTIO_STATUS_FEATURES_OK = 8u;
constexpr mx_u32_t VIRTIO_STATUS_FAILED = 128u;

constexpr mx_u16_t VRING_DESC_F_NEXT = 1u;
constexpr mx_u16_t VRING_DESC_F_WRITE = 2u;
constexpr mx_u16_t VRING_AVAIL_F_NO_INTERRUPT = 1u;

constexpr mx_u32_t VIRTIO_BLK_T_IN = 0;
constexpr mx_u32_t VIRTIO_BLK_T_OUT = 1;
constexpr mx_u32_t VIRTIO_BLK_T_FLUSH = 4;
constexpr mx_u8_t VIRTIO_BLK_S_OK = 0;

struct VringDesc {
    mx_u64_t addr;
    mx_u32_t len;
    mx_u16_t flags;
    mx_u16_t next;
};
static_assert(sizeof(VringDesc) == 16);

struct VringAvail {
    mx_u16_t flags;
    mx_u16_t idx;
    mx_u16_t ring[kQueueSize];
};

struct VringUsedElem {
    mx_u32_t id;
    mx_u32_t len;
};
static_assert(sizeof(VringUsedElem) == 8);

struct VringUsed {
    mx_u16_t flags;
    mx_u16_t idx;
    VringUsedElem ring[kQueueSize];
};

struct VirtioBlkHeader {
    mx_u32_t type;
    mx_u32_t reserved;
    mx_u64_t sector;
};
static_assert(sizeof(VirtioBlkHeader) == 16);

static VringDesc* g_desc = nullptr;
static VringAvail* g_avail = nullptr;
static VringUsed* g_used = nullptr;
static VirtioBlkHeader* g_req_hdr = nullptr;
static mx_u8_t* g_req_status = nullptr;
static mx_u64_t g_io_data_offset = 0;

static void barrier() {
    libk::cpu::data_sync();
}

static mx_u64_t dma_phys(const void* ptr) {
    return g_dma_phys + (reinterpret_cast<mx_u64_t>(ptr) -
                         reinterpret_cast<mx_u64_t>(g_dma_base));
}

static volatile mx_u32_t* reg32(mx_u32_t off) {
    return reinterpret_cast<volatile mx_u32_t*>(g_mmio + off);
}

static mx_u32_t mmio_read32(mx_u32_t off) {
    barrier();
    return *reg32(off);
}

static void mmio_write32(mx_u32_t off, mx_u32_t value) {
    *reg32(off) = value;
    barrier();
}

static void mmio_write64_pair(mx_u32_t low_off, mx_u64_t value) {
    mmio_write32(low_off, static_cast<mx_u32_t>(value));
    mmio_write32(low_off + 4, static_cast<mx_u32_t>(value >> 32));
}

static mx_u32_t virtio_read_features(mx_u32_t selector) {
    mmio_write32(VIRTIO_MMIO_HOST_FEATURES_SEL, selector);
    return mmio_read32(VIRTIO_MMIO_HOST_FEATURES);
}

static void virtio_write_features(mx_u32_t selector, mx_u32_t features) {
    mmio_write32(VIRTIO_MMIO_GUEST_FEATURES_SEL, selector);
    mmio_write32(VIRTIO_MMIO_GUEST_FEATURES, features);
}

static void virtio_add_status(mx_u32_t status) {
    mmio_write32(VIRTIO_MMIO_STATUS, mmio_read32(VIRTIO_MMIO_STATUS) | status);
}

static void virtio_fail() {
    if (g_mmio) {
        mmio_write32(VIRTIO_MMIO_STATUS,
                     mmio_read32(VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
    }
    g_blk_ready = false;
}

static mx_u64_t virtio_read_config64(mx_u32_t offset) {
    const mx_u32_t lo = mmio_read32(VIRTIO_MMIO_CONFIG + offset);
    const mx_u32_t hi = mmio_read32(VIRTIO_MMIO_CONFIG + offset + 4);
    return static_cast<mx_u64_t>(lo) | (static_cast<mx_u64_t>(hi) << 32);
}

static bool virtio_queue_init() {
    mmio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    if (!g_legacy_transport && mmio_read32(VIRTIO_MMIO_QUEUE_READY) != 0) {
        mmio_write32(VIRTIO_MMIO_QUEUE_READY, 0);
    }

    const mx_u32_t queue_max = mmio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < kQueueSize) return false;

    if (g_legacy_transport) {
        mx_u8_t* queue = static_cast<mx_u8_t*>(g_dma_arena.alloc_zeroed(8192, 4096));
        if (!queue) return false;
        const mx_u64_t avail_offset = sizeof(VringDesc) * kQueueSize;
        const mx_u64_t used_offset = 4096;
        g_desc = reinterpret_cast<VringDesc*>(queue);
        g_avail = reinterpret_cast<VringAvail*>(queue + avail_offset);
        g_used = reinterpret_cast<VringUsed*>(queue + used_offset);
        mmio_write32(VIRTIO_MMIO_QUEUE_NUM, kQueueSize);
        mmio_write32(VIRTIO_MMIO_QUEUE_ALIGN, 4096);
        mmio_write32(VIRTIO_MMIO_QUEUE_PFN,
                     static_cast<mx_u32_t>(dma_phys(queue) >> 12));
    } else {
        g_desc = static_cast<VringDesc*>(g_dma_arena.alloc_zeroed(sizeof(VringDesc) * kQueueSize, 16));
        g_avail = static_cast<VringAvail*>(g_dma_arena.alloc_zeroed(sizeof(VringAvail), 16));
        g_used = static_cast<VringUsed*>(g_dma_arena.alloc_zeroed(sizeof(VringUsed), 16));
        if (!g_desc || !g_avail || !g_used) return false;
        mmio_write32(VIRTIO_MMIO_QUEUE_NUM, kQueueSize);
        mmio_write64_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, dma_phys(g_desc));
        mmio_write64_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, dma_phys(g_avail));
        mmio_write64_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, dma_phys(g_used));
        mmio_write32(VIRTIO_MMIO_QUEUE_READY, 1);
    }

    g_req_hdr = static_cast<VirtioBlkHeader*>(g_dma_arena.alloc_zeroed(sizeof(VirtioBlkHeader), 16));
    g_req_status = static_cast<mx_u8_t*>(g_dma_arena.alloc_zeroed(1, 1));
    if (g_dma_arena.used() > BLK_DMA_CLIENT_OFFSET ||
        !g_dma_arena.alloc(BLK_DMA_CLIENT_OFFSET - g_dma_arena.used(), 1)) return false;
    void* data_region = g_dma_arena.alloc_zeroed(kBlkSectorSize * 8, kBlkSectorSize);
    if (!g_desc || !g_avail || !g_used || !g_req_hdr || !g_req_status || !data_region) {
        return false;
    }

    g_data_region_offset =
        reinterpret_cast<mx_u64_t>(data_region) - reinterpret_cast<mx_u64_t>(g_dma_base);
    g_io_data_offset = g_data_region_offset;
    g_avail_idx = 0;
    g_used_idx = 0;
    g_avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    g_avail->idx = 0;
    g_used->flags = 0;
    g_used->idx = 0;

    return true;
}

static mx_i32_t virtio_submit(mx_u32_t type, mx_u64_t sector,
                              mx_u64_t data_offset, mx_u32_t byte_count) {
    if (!g_blk_ready && type != VIRTIO_BLK_T_FLUSH) {
        return static_cast<mx_i32_t>(MX_ERR_SERVICE_UNAVAILABLE);
    }
    if (!g_desc || !g_avail || !g_used || !g_req_hdr || !g_req_status) {
        return static_cast<mx_i32_t>(MX_ERR_SERVICE_UNAVAILABLE);
    }
    if (type != VIRTIO_BLK_T_FLUSH) {
        if (byte_count == 0) {
            mochi::log_raw_marker("drv.blk.virtio: submit invalid byte count\n");
            return static_cast<mx_i32_t>(MX_ERR_INVALID);
        }
        if (data_offset < g_data_region_offset || byte_count > g_dma_size ||
            data_offset > g_dma_size - byte_count) {
            mochi::log_raw_marker("drv.blk.virtio: submit invalid dma range\n");
            return static_cast<mx_i32_t>(MX_ERR_INVALID);
        }
    }

    g_req_hdr->type = type;
    g_req_hdr->reserved = 0;
    g_req_hdr->sector = sector;
    *g_req_status = 0xFFu;

    g_desc[0].addr = dma_phys(g_req_hdr);
    g_desc[0].len = sizeof(VirtioBlkHeader);
    g_desc[0].flags = VRING_DESC_F_NEXT;
    g_desc[0].next = (type == VIRTIO_BLK_T_FLUSH) ? 2 : 1;

    if (type != VIRTIO_BLK_T_FLUSH) {
        g_desc[1].addr = g_dma_phys + data_offset;
        g_desc[1].len = byte_count;
        g_desc[1].flags = VRING_DESC_F_NEXT;
        if (type == VIRTIO_BLK_T_IN) g_desc[1].flags |= VRING_DESC_F_WRITE;
        g_desc[1].next = 2;
    }

    g_desc[2].addr = dma_phys(g_req_status);
    g_desc[2].len = 1;
    g_desc[2].flags = VRING_DESC_F_WRITE;
    g_desc[2].next = 0;

    barrier();
    g_avail->ring[g_avail_idx % kQueueSize] = 0;
    barrier();
    ++g_avail_idx;
    g_avail->idx = g_avail_idx;
    barrier();
    mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    for (mx_u32_t spin = 0; spin < kVirtioWaitSpins; ++spin) {
        barrier();
        if (g_used->idx != g_used_idx) break;
        if ((spin & 0x3FFu) == 0) mx_yield();
    }
    barrier();
    if (g_used->idx == g_used_idx) {
        mochi::log_raw_marker("drv.blk.virtio: submit timeout\n");
        return static_cast<mx_i32_t>(MX_ERR_BUSY);
    }

    const VringUsedElem& used = g_used->ring[g_used_idx % kQueueSize];
    ++g_used_idx;
    (void)mmio_read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    mmio_write32(VIRTIO_MMIO_INTERRUPT_ACK, 0x3);
    barrier();

    if (used.id != 0) {
        mochi::log_raw_marker("drv.blk.virtio: bad used descriptor id\n");
        return static_cast<mx_i32_t>(MX_ERR_FAULT);
    }
    if (*g_req_status != VIRTIO_BLK_S_OK) {
        mochi::log_raw_marker("drv.blk.virtio: device status error\n");
        return static_cast<mx_i32_t>(MX_ERR_FAULT);
    }
    return MX_OK;
}

static bool virtio_init() {
    if (!g_mmio || !g_dma_base || g_dma_size == 0) return false;
    const mx_u32_t version = mmio_read32(VIRTIO_MMIO_VERSION);
    if (version == 1) {
        g_legacy_transport = true;
        mochi::log_raw_marker("drv.blk.virtio: legacy transport\n");
    } else if (version == 2) {
        g_legacy_transport = false;
    } else {
        mochi::log_raw_marker("drv.blk.virtio: unsupported transport version\n");
        return false;
    }

    mmio_write32(VIRTIO_MMIO_STATUS, 0);
    for (mx_u32_t spin = 0; spin < 1024; ++spin) {
        if (mmio_read32(VIRTIO_MMIO_STATUS) == 0) break;
        mx_yield();
    }

    if (mmio_read32(VIRTIO_MMIO_STATUS) != 0) return false;

    virtio_add_status(VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_add_status(VIRTIO_STATUS_DRIVER);
    if (g_legacy_transport) {
        mmio_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }

    const mx_u32_t dev_features0 = virtio_read_features(0);
    const mx_u32_t driver_features0 = dev_features0 & VIRTIO_BLK_F_FLUSH;
    g_flush_supported = (driver_features0 & VIRTIO_BLK_F_FLUSH) != 0;
    virtio_write_features(0, driver_features0);

    if (!g_legacy_transport) {
        const mx_u32_t dev_features1 = virtio_read_features(1);
        if ((dev_features1 & (1u << VIRTIO_F_VERSION_1_BIT)) == 0) {
            mochi::log_raw_marker("drv.blk.virtio: VERSION_1 feature missing\n");
            virtio_fail();
            return false;
        }
        virtio_write_features(1, (1u << VIRTIO_F_VERSION_1_BIT));
        virtio_add_status(VIRTIO_STATUS_FEATURES_OK);
        if ((mmio_read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
            mochi::log_raw_marker("drv.blk.virtio: feature negotiation failed\n");
            virtio_fail();
            return false;
        }
    }

    if (!virtio_queue_init()) {
        mochi::log_raw_marker("drv.blk.virtio: queue setup failed\n");
        virtio_fail();
        return false;
    }

    g_capacity_sectors = virtio_read_config64(0);
    virtio_add_status(VIRTIO_STATUS_DRIVER_OK);
    g_blk_ready = true;
    return true;
}

static void virtio_self_test_read() {
    if (!g_blk_ready || g_capacity_sectors == 0) return;
    const mx_i32_t rc =
        virtio_submit(VIRTIO_BLK_T_IN, 0, g_io_data_offset, kBlkSectorSize);
    if (rc == MX_OK) {
        mochi::log_raw_marker("drv.blk.virtio: BLK read OK\n");
    } else {
        mochi::log_raw_marker("drv.blk.virtio: BLK read failed\n");
    }
}

static bool virtio_blk_present(const MxBootInfo* boot) {
    const mx_handle_t mmio = mochi::boot::handle(boot, "virtio.blk.mmio");
    if (mmio == MX_NULL_HANDLE) return false;
    if (mx_map_mmio(mmio, kVaVirtioMmio, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
        mochi::log_raw_marker("drv.blk.virtio: mmio map failed\n");
        return false;
    }
    for (mx_u32_t off = 0; off < kVirtioMmioScanSize; off += kVirtioMmioSlotSize) {
        volatile auto* regs =
            reinterpret_cast<volatile mx_u32_t*>(kVaVirtioMmio + off);
        const mx_u32_t magic = regs[VIRTIO_MMIO_MAGIC / sizeof(mx_u32_t)];
        const mx_u32_t device = regs[VIRTIO_MMIO_DEVICE_ID / sizeof(mx_u32_t)];
        if (magic == 0x74726976u && device == 2) {
            g_virtio_slot_offset = off;
            return true;
        }
    }
    mochi::log_raw_marker("drv.blk.virtio: mmio bank has no virtio-blk\n");
    return false;
}

static void handle_request(const mochi::fs::BlkServer& blk, const RingSlot& slot,
                           bool dma_ready) {
    BlkRequest req{};
    if (!mochi::message_get(slot, &req)) {
        (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL), 0);
        return;
    }

    if (slot.type != req.type) {
        (void)blk.send_reply(MX_ERR_BAD_PROTOCOL, 0);
        return;
    }
    switch (req.type) {
    case BLK_FLUSH: {
        if (!g_blk_ready || !dma_ready) {
            (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_SERVICE_UNAVAILABLE), 0);
        } else if (!g_flush_supported) {
            (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_NOT_IMPLEMENTED), 0);
        } else {
            const mx_i32_t rc = virtio_submit(VIRTIO_BLK_T_FLUSH, 0, 0, 0);
            (void)blk.send_reply(rc, 0);
        }
        break;
    }
    case BLK_READ:
    case BLK_WRITE: {
        if (!g_blk_ready) {
            (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_SERVICE_UNAVAILABLE), 0);
        } else if (!dma_ready) {
            (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_SERVICE_UNAVAILABLE), 0);
        } else if (req.sector_count == 0 ||
                   req.sector_count > (0xFFFF'FFFFu / kBlkSectorSize)) {
            (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_INVALID), 0);
        } else if (g_capacity_sectors != 0 &&
                   (req.sector > g_capacity_sectors ||
                    req.sector_count > g_capacity_sectors - req.sector)) {
            (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_OVERFLOW), 0);
        } else {
            const mx_u32_t byte_count = req.sector_count * kBlkSectorSize;
            const mx_u32_t virtio_type =
                (req.type == BLK_READ) ? VIRTIO_BLK_T_IN : VIRTIO_BLK_T_OUT;
            const mx_i32_t rc =
                virtio_submit(virtio_type, req.sector, req.arena_offset, byte_count);
            (void)blk.send_reply(rc, rc == MX_OK ? req.sector_count : 0);
        }
        break;
    }
    default:
        (void)blk.send_reply(static_cast<mx_i32_t>(MX_ERR_BAD_PROTOCOL), 0);
        break;
    }
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.blk.virtio");

    const mochi::fs::BlkServer blk{mochi::boot::map_endpoint(
        boot, {"blk.reply.ring", kVaReplyRing, "blk.reply.doorbell"},
        {"blk.req.ring", kVaReqRing, "blk.req.doorbell"})};

    const bool device_present = virtio_blk_present(boot);
    if (device_present) {
        g_mmio = reinterpret_cast<volatile mx_u8_t*>(kVaVirtioMmio + g_virtio_slot_offset);
    }
    MxCapInfo dma_info{};
    const bool dma_ready = mochi::driver::map_dma(boot, "virtio.blk.dma", kVaVirtioDma, &dma_info);
    if (dma_ready) {
        g_dma_base = reinterpret_cast<mx_u8_t*>(kVaVirtioDma);
        g_dma_phys = dma_info.base;
        g_dma_size = dma_info.size;
        g_dma_arena = mochi::Arena(g_dma_base, g_dma_size);
    }

    if (device_present) {
        if (g_virtio_slot_offset != 0) {
            mochi::log_raw_marker("drv.blk.virtio: virtio-blk found after slot scan\n");
        }
        mochi::log_raw_marker("drv.blk.virtio: virtio-blk device detected\n");
    } else {
        mochi::log_raw_marker("drv.blk.virtio: no virtio-blk caps; no-device mode\n");
    }
    if (!dma_ready) {
        mochi::log_raw_marker("drv.blk.virtio: no dma arena\n");
    }
    if (device_present && dma_ready) {
        if (virtio_init()) {
            mochi::log_raw_marker("drv.blk.virtio: queue ready\n");
            virtio_self_test_read();
        } else {
            mochi::log_raw_marker("drv.blk.virtio: init failed\n");
        }
    }

    if (!blk.valid()) {
        mochi::log_raw_marker("drv.blk.virtio: no block endpoint\n");
        mochi::park();
    }

    mochi::log_raw_marker("drv.blk.virtio: ready\n");

    mochi::boot::ready(boot);
    while (true) {
        RingSlot slot{};
        while (blk.try_recv(&slot)) {
            handle_request(blk, slot, dma_ready);
        }
        if (blk.wait_recv(&slot)) {
            handle_request(blk, slot, dma_ready);
        } else {
            mx_yield();
        }
    }
}

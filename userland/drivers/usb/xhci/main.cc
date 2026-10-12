// userland/drivers/usb/xhci/main.cc - drv.usb.xhci service.
//
// Maps xHCI MMIO via SYS_MAP_MMIO, initializes the host controller, enumerates
// QEMU's root ports, configures a HID boot keyboard interrupt-IN endpoint, and
// forwards 8-byte boot keyboard reports to drv.usb.hid.
//
// This milestone deliberately polls the xHCI event ring. IRQ routing/ack is
// left for the next hardening pass.

#include "mochi/syscall.h"
#include "mochi/arena.h"
#include "libk/bytes.h"
#include "libk/endian.h"
#include "libk/integer.h"
#include "libk/dma.h"
#include "libk/mmio.h"
#include "midix/usb/descriptors.h"
#include "midix/usb/xhci_ring.h"
#include "midix/usb/xhci_events.h"
#include "mochi/ring.h"
#include "mochi/log.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/usb.h"
#include "mochi/va_layout.h"

using namespace mochi::va::xhci;

constexpr uint32_t kControlBufferBytes = 512;

// xHCI capability register offsets.
constexpr uint32_t kXhciCapLength  = 0x00;
constexpr uint32_t kXhciHcsParams1 = 0x04;
constexpr uint32_t kXhciHcsParams2 = 0x08;
constexpr uint32_t kXhciHccParams1 = 0x10;
constexpr uint32_t kXhciDbOff      = 0x14;
constexpr uint32_t kXhciRtsoff     = 0x18;

// xHCI operational register offsets, relative to CAPLENGTH.
constexpr uint32_t kXhciUsbCmd  = 0x00;
constexpr uint32_t kXhciUsbSts  = 0x04;
constexpr uint32_t kXhciPageSize = 0x08;
constexpr uint32_t kXhciCrcr    = 0x18;
constexpr uint32_t kXhciDcbaap  = 0x30;
constexpr uint32_t kXhciConfig  = 0x38;
constexpr uint32_t kXhciPorts   = 0x400;

// Runtime interrupter 0 registers, relative to RTSOFF + 0x20.
constexpr uint32_t kIrqIman  = 0x00;
constexpr uint32_t kIrqErstsz = 0x08;
constexpr uint32_t kIrqErstba = 0x10;
constexpr uint32_t kIrqErdp   = 0x18;

constexpr uint32_t kCmdRun      = 1u << 0;
[[maybe_unused]] constexpr uint32_t kCmdHcReset  = 1u << 1;
constexpr uint32_t kCmdInte     = 1u << 2;
constexpr uint32_t kStsHcHalted = 1u << 0;
constexpr uint32_t kStsNotReady = 1u << 11;

constexpr uint32_t kPortCcs = 1u << 0;
constexpr uint32_t kPortPed = 1u << 1;
constexpr uint32_t kPortPr  = 1u << 4;
constexpr uint32_t kPortPp  = 1u << 9;
constexpr uint32_t kPortChangeMask =
    (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) |
    (1u << 21) | (1u << 22) | (1u << 23);

constexpr uint32_t kTrbCycle = 1u << 0;
constexpr uint32_t kTrbIsp = 1u << 2;
constexpr uint32_t kTrbChain = 1u << 4;
constexpr uint32_t kTrbIoc = 1u << 5;
constexpr uint32_t kTrbIdt = 1u << 6;

enum TrbType : uint32_t {
    TRB_NORMAL = 1,
    TRB_SETUP_STAGE = 2,
    TRB_DATA_STAGE = 3,
    TRB_STATUS_STAGE = 4,
    TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9,
    TRB_DISABLE_SLOT = 10,
    TRB_ADDRESS_DEVICE = 11,
    TRB_CONFIGURE_ENDPOINT = 12,
    TRB_EVALUATE_CONTEXT = 13,
    TRB_TRANSFER_EVENT = 32,
    TRB_COMMAND_COMPLETION_EVENT = 33,
    TRB_PORT_STATUS_CHANGE_EVENT = 34,
};

enum CompletionCode : uint32_t {
    CC_SUCCESS = 1,
    CC_SHORT_PACKET = 13,
};

using Trb = midix::usb::xhci::Trb;

struct ErstEntry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
};
static_assert(sizeof(ErstEntry) == 16);

using XhciRing = midix::usb::xhci::ProducerRing;

struct UsbDevice {
    uint8_t port;
    uint8_t slot_id;
    uint8_t speed;
    uint8_t config_value;
    uint8_t interface_number;
    uint8_t endpoint_address;
    uint8_t endpoint_id;
    uint8_t interval;
    uint16_t endpoint_mps;
    XhciRing control_ring;
    uint8_t* input_context;
    uint64_t input_context_phys;
    uint8_t* output_context;
    uint64_t output_context_phys;
    uint8_t* control_buffer;
    uint64_t control_buffer_phys;
};

struct KeyboardState {
    bool ready;
    bool report_pending;
    uint8_t slot_id;
    uint8_t endpoint_id;
    XhciRing interrupt_ring;
    UsbBootKeyboardReport* report;
    uint64_t report_phys;
};

static libk::Mmio g_mmio{};
static uint32_t g_cap_len = 0;
static uint32_t g_dboff = 0;
static uint32_t g_rtsoff = 0;
static uint32_t g_max_slots = 0;
static uint32_t g_num_ports = 0;
static uint32_t g_ctx_stride = 32;

static uint8_t* g_dma_base = nullptr;
static uint64_t g_dma_phys = 0;
static uint64_t g_dma_size = 0;
static mochi::Arena g_dma_arena;

static uint64_t* g_dcbaa = nullptr;
static XhciRing g_command_ring{};
static Trb* g_event_ring = nullptr;
static uint64_t g_event_ring_phys = 0;
static uint32_t g_event_count = 0;
static uint32_t g_event_index = 0;
static uint32_t g_event_cycle = 1;
static ErstEntry* g_erst = nullptr;
static uint64_t g_erst_phys = 0;

static mochi::usb::BootKeyboardReportSink g_hid{};
static KeyboardState g_keyboard{};
static midix::usb::xhci::EventBacklog g_deferred_events{};
static bool g_event_fault = false;

static void barrier() {
    libk::dma::order_mmio();
}

static uint32_t trb_type(uint32_t type) {
    return type << 10;
}

static uint32_t event_type(const Trb& trb) {
    return (trb.control >> 10) & 0x3Fu;
}

static uint32_t completion_code(const Trb& trb) {
    return (trb.status >> 24) & 0xFFu;
}

static uint8_t event_slot(const Trb& trb) {
    return static_cast<uint8_t>((trb.control >> 24) & 0xFFu);
}

static uint8_t event_endpoint(const Trb& trb) {
    return static_cast<uint8_t>((trb.control >> 16) & 0x1Fu);
}

static uint32_t mmio_read32(uint64_t off) {
    barrier();
    uint32_t value = 0;
    if (!g_mmio.read32(off, &value)) g_event_fault = true;
    return value;
}

static void mmio_write32(uint64_t off, uint32_t val) {
    if (!g_mmio.write32(off, val)) g_event_fault = true;
    barrier();
}

static void mmio_write64(uint64_t off, uint64_t val) {
    if (!g_mmio.write64(off, val)) g_event_fault = true;
    barrier();
}

static uint32_t cap_read(uint32_t off) {
    return mmio_read32(off);
}

static uint32_t op_read(uint32_t off) {
    return mmio_read32(g_cap_len + off);
}

static void op_write(uint32_t off, uint32_t val) {
    mmio_write32(g_cap_len + off, val);
}

static void op_write64(uint32_t off, uint64_t val) {
    mmio_write64(g_cap_len + off, val);
}

static uint64_t irq0_off(uint32_t off) {
    return static_cast<uint64_t>(g_rtsoff) + 0x20ull + off;
}

static void irq0_write32(uint32_t off, uint32_t val) {
    mmio_write32(irq0_off(off), val);
}

static void irq0_write64(uint32_t off, uint64_t val) {
    mmio_write64(irq0_off(off), val);
}

static void doorbell_write(uint8_t slot_id, uint32_t target) {
    mmio_write32(static_cast<uint64_t>(g_dboff) + static_cast<uint64_t>(slot_id) * 4ull,
                 target);
}

static uint32_t port_offset(uint8_t port) {
    return g_cap_len + kXhciPorts + (static_cast<uint32_t>(port - 1) * 0x10u);
}

static uint32_t port_read(uint8_t port) {
    return mmio_read32(port_offset(port));
}

static void port_write(uint8_t port, uint32_t val) {
    mmio_write32(port_offset(port), val & ~kPortChangeMask);
}

static void port_clear_changes(uint8_t port) {
    const uint32_t s = port_read(port);
    mmio_write32(port_offset(port), (s & ~kPortChangeMask) | kPortChangeMask);
}

static uint64_t dma_phys(void* ptr) {
    return g_dma_phys + (reinterpret_cast<uint64_t>(ptr) -
                         reinterpret_cast<uint64_t>(g_dma_base));
}

static uint32_t* input_context_dwords(UsbDevice& dev, uint32_t dci) {
    return reinterpret_cast<uint32_t*>(dev.input_context +
                                       static_cast<uint64_t>(dci + 1u) * g_ctx_stride);
}

static uint32_t* output_context_dwords(UsbDevice& dev, uint32_t dci) {
    return reinterpret_cast<uint32_t*>(dev.output_context +
                                       static_cast<uint64_t>(dci) * g_ctx_stride);
}

static bool ring_init(XhciRing& ring, uint32_t count, bool link) {
    if (!link || count < 4 || count > 4096 || (count & (count - 1))) return false;
    // Align the segment to its power-of-two extent so it cannot straddle the
    // 64 KiB boundary, regardless of the physical arena's starting page.
    const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(Trb);
    auto* storage = static_cast<Trb*>(g_dma_arena.alloc_zeroed(bytes, bytes));
    return storage && ring.init(storage, dma_phys(storage), count);
}

static uint64_t ring_enqueue(XhciRing& ring, Trb trb) {
    return ring.enqueue(trb);
}

static void event_ring_update_erdp() {
    const uint64_t ptr = g_event_ring_phys +
                         static_cast<uint64_t>(g_event_index) * sizeof(Trb);
    irq0_write64(kIrqErdp, ptr | (1ull << 3));
}

static bool event_poll(Trb* out) {
    Trb& src = g_event_ring[g_event_index];
    const uint32_t control = __atomic_load_n(&src.control, __ATOMIC_ACQUIRE);
    if ((control & kTrbCycle) != (g_event_cycle ? kTrbCycle : 0)) return false;
    libk::dma::consume();

    out->parameter = src.parameter;
    out->status = src.status;
    out->control = control;

    ++g_event_index;
    if (g_event_index == g_event_count) {
        g_event_index = 0;
        g_event_cycle ^= 1u;
    }
    event_ring_update_erdp();
    return true;
}

static bool wait_command(uint64_t command_phys, Trb* out_event) {
    for (uint32_t spin = 0; spin < 500000; ++spin) {
        Trb event{};
        while (event_poll(&event)) {
            if (event_type(event) == TRB_COMMAND_COMPLETION_EVENT &&
                event.parameter == command_phys) {
                if (out_event) *out_event = event;
                return completion_code(event) == CC_SUCCESS;
            }
            if (!g_deferred_events.push(event)) { g_event_fault = true; return false; }
        }
        mx_yield();
    }
    return false;
}

static bool issue_command(Trb command, Trb* out_event) {
    const uint64_t phys = ring_enqueue(g_command_ring, command);
    if (!phys) return false;
    doorbell_write(0, 0);
    Trb event{};
    const bool success = wait_command(phys, &event);
    if (event.parameter == phys) (void)g_command_ring.retire(1);
    if (out_event) *out_event = event;
    return success;
}

static bool wait_transfer(uint8_t slot_id, uint8_t endpoint_id, uint64_t status_phys,
                          uint64_t data_phys, uint32_t requested, uint32_t* out_actual, uint32_t* out_code) {
    uint32_t actual = requested;
    for (uint32_t spin = 0; spin < 500000; ++spin) {
        Trb event{};
        while (event_poll(&event)) {
            if (event_type(event) != TRB_TRANSFER_EVENT || event_slot(event) != slot_id ||
                event_endpoint(event) != endpoint_id ||
                (event.parameter != status_phys && (!data_phys || event.parameter != data_phys))) {
                if (!g_deferred_events.push(event)) { g_event_fault = true; return false; }
                continue;
            }
            const uint32_t code = completion_code(event);
            if (event.parameter == data_phys && code == CC_SHORT_PACKET) {
                const auto residual = event.status & 0xffffffu;
                if (residual > requested) return false;
                actual = requested - residual;
                continue; // the status stage still owns descriptors and DMA
            }
            if (out_code) *out_code = code;
            if (out_actual) *out_actual = actual;
            return code == CC_SUCCESS || code == CC_SHORT_PACKET;
        }
        mx_yield();
    }
    return false;
}

static bool xhci_reset() {
    if (!(op_read(kXhciUsbSts) & kStsHcHalted)) {
        op_write(kXhciUsbCmd, op_read(kXhciUsbCmd) & ~kCmdRun);
        uint32_t attempts = 100000;
        while (!(op_read(kXhciUsbSts) & kStsHcHalted) && --attempts) {
            asm volatile("yield" ::: "memory");
        }
        if (!attempts) return false;
    }

    uint32_t attempts = 100000;
#ifndef ANKO_XHCI_NORESET
    // T210 keeps Hekate's FALCON firmware; other controllers reset normally.
    op_write(kXhciUsbCmd, op_read(kXhciUsbCmd) | kCmdHcReset);
    while ((op_read(kXhciUsbCmd) & kCmdHcReset) && --attempts) {
        asm volatile("yield" ::: "memory");
    }
    if (!attempts) return false;
    attempts = 100000;
#endif
    while ((op_read(kXhciUsbSts) & kStsNotReady) && --attempts) {
        asm volatile("yield" ::: "memory");
    }
    return attempts > 0;
}

static uint32_t scratchpad_count(uint32_t hcs2) {
    const uint32_t lo = (hcs2 >> 27) & 0x1Fu;
    const uint32_t hi = (hcs2 >> 21) & 0x1Fu;
    return (hi << 5) | lo;
}

static bool xhci_dma_init() {
    g_dcbaa = static_cast<uint64_t*>(g_dma_arena.alloc_zeroed(256 * sizeof(uint64_t), 64));
    if (!g_dcbaa) return false;

    const uint32_t hcs2 = cap_read(kXhciHcsParams2);
    const uint32_t scratchpads = scratchpad_count(hcs2);
    if (scratchpads != 0) {
        if (scratchpads > g_dma_arena.remaining() / 4096) return false;
        auto* scratchpad_array =
            static_cast<uint64_t*>(g_dma_arena.alloc_zeroed(static_cast<uint64_t>(scratchpads) *
                                             sizeof(uint64_t), 64));
        if (!scratchpad_array) return false;
        for (uint32_t i = 0; i < scratchpads; ++i) {
            void* page = g_dma_arena.alloc_zeroed(4096, 4096);
            if (!page) return false;
            scratchpad_array[i] = dma_phys(page);
        }
        g_dcbaa[0] = dma_phys(scratchpad_array);
    }

    if (!ring_init(g_command_ring, 64, true)) return false;

    g_event_count = 128;
    g_event_ring = static_cast<Trb*>(
        g_dma_arena.alloc_zeroed(static_cast<uint64_t>(g_event_count) * sizeof(Trb), 64));
    if (!g_event_ring) return false;
    g_event_ring_phys = dma_phys(g_event_ring);
    g_event_index = 0;
    g_event_cycle = 1;

    g_erst = static_cast<ErstEntry*>(g_dma_arena.alloc_zeroed(sizeof(ErstEntry), 64));
    if (!g_erst) return false;
    g_erst_phys = dma_phys(g_erst);
    g_erst[0].base = g_event_ring_phys;
    g_erst[0].size = g_event_count;
    g_erst[0].reserved = 0;
    barrier();
    return true;
}

static bool xhci_program_and_start() {
    const uint32_t page_size = op_read(kXhciPageSize);
    if ((page_size & 1u) == 0) return false;

    op_write64(kXhciDcbaap, dma_phys(g_dcbaa));
    op_write64(kXhciCrcr, g_command_ring.phys | 1ull);

    irq0_write32(kIrqIman, 0);
    irq0_write32(kIrqErstsz, 1);
    irq0_write64(kIrqErstba, g_erst_phys);
    irq0_write64(kIrqErdp, g_event_ring_phys | (1ull << 3));
    irq0_write32(kIrqIman, 2);

    op_write(kXhciConfig, g_max_slots);
    op_write(kXhciUsbCmd, kCmdRun | kCmdInte);

    uint32_t attempts = 100000;
    while ((op_read(kXhciUsbSts) & kStsHcHalted) && --attempts) {
        asm volatile("yield" ::: "memory");
    }
    return attempts > 0;
}

static bool reset_port(uint8_t port, uint8_t* out_speed) {
    uint32_t s = port_read(port);
    if ((s & kPortCcs) == 0) return false;

    if ((s & kPortPp) == 0) {
        port_write(port, s | kPortPp);
        for (uint32_t i = 0; i < 10000; ++i) asm volatile("yield" ::: "memory");
        s = port_read(port);
    }

    port_clear_changes(port);
    s = port_read(port);
    port_write(port, s | kPortPr);

    uint32_t attempts = 250000;
    while ((port_read(port) & kPortPr) && --attempts) {
        asm volatile("yield" ::: "memory");
    }
    if (!attempts) return false;

    attempts = 250000;
    while (((s = port_read(port)) & kPortPed) == 0 && --attempts) {
        asm volatile("yield" ::: "memory");
    }
    if (!attempts) return false;

    port_clear_changes(port);
    if (out_speed) *out_speed = static_cast<uint8_t>((s >> 10) & 0xFu);
    return true;
}

static uint16_t default_ep0_mps(uint8_t speed) {
    if (speed == 4) return 512; // SuperSpeed
    if (speed == 3) return 64;  // High speed
    return 8;                   // Full/low speed default pipe
}

static bool device_alloc(UsbDevice& dev) {
    const uint64_t context_bytes = static_cast<uint64_t>(33u) * g_ctx_stride;
    dev.input_context = static_cast<uint8_t*>(g_dma_arena.alloc_zeroed(context_bytes, 64));
    dev.output_context = static_cast<uint8_t*>(g_dma_arena.alloc_zeroed(static_cast<uint64_t>(32u) *
                                                         g_ctx_stride, 64));
    dev.control_buffer = static_cast<uint8_t*>(g_dma_arena.alloc_zeroed(kControlBufferBytes, 64));
    if (!dev.input_context || !dev.output_context || !dev.control_buffer) return false;
    dev.input_context_phys = dma_phys(dev.input_context);
    dev.output_context_phys = dma_phys(dev.output_context);
    dev.control_buffer_phys = dma_phys(dev.control_buffer);
    return ring_init(dev.control_ring, 64, true);
}

static void prepare_address_context(UsbDevice& dev, uint16_t ep0_mps) {
    const uint64_t context_bytes = static_cast<uint64_t>(33u) * g_ctx_stride;
    libk::zero(dev.input_context, context_bytes);

    auto* icc = reinterpret_cast<uint32_t*>(dev.input_context);
    icc[0] = 0;                         // Drop Context flags
    icc[1] = (1u << 0) | (1u << 1);     // Slot + EP0 contexts

    uint32_t* slot = input_context_dwords(dev, 0);
    slot[0] = (static_cast<uint32_t>(dev.speed) << 20) | (1u << 27);
    slot[1] = static_cast<uint32_t>(dev.port) << 16;
    slot[2] = 0;
    slot[3] = 0;

    uint32_t* ep0 = input_context_dwords(dev, 1);
    ep0[0] = 0;
    ep0[1] = (3u << 1) | (4u << 3) | (static_cast<uint32_t>(ep0_mps) << 16);
    const uint64_t dequeue = dev.control_ring.phys | 1ull;
    ep0[2] = static_cast<uint32_t>(dequeue);
    ep0[3] = static_cast<uint32_t>(dequeue >> 32);
    ep0[4] = 8;
    barrier();
}

static bool address_device(UsbDevice& dev) {
    Trb event{};
    Trb enable{};
    enable.control = trb_type(TRB_ENABLE_SLOT);
    if (!issue_command(enable, &event)) return false;

    dev.slot_id = event_slot(event);
    if (dev.slot_id == 0) return false;

    g_dcbaa[dev.slot_id] = dev.output_context_phys;
    prepare_address_context(dev, default_ep0_mps(dev.speed));

    Trb address{};
    address.parameter = dev.input_context_phys;
    address.control = trb_type(TRB_ADDRESS_DEVICE) |
                      (static_cast<uint32_t>(dev.slot_id) << 24);
    return issue_command(address, &event);
}

static bool usb_control(UsbDevice& dev, uint8_t request_type, uint8_t request,
                        uint16_t value, uint16_t index, uint8_t* buffer,
                        uint16_t length, bool direction_in) {
    if (length > kControlBufferBytes || (length && !buffer)) return false;
    const uint32_t descriptors = length ? 3 : 2;
    if (dev.control_ring.available() < descriptors) return false;
    if (direction_in) libk::zero(dev.control_buffer, kControlBufferBytes);
    const uint64_t setup =
        static_cast<uint64_t>(request_type) |
        (static_cast<uint64_t>(request) << 8) |
        (static_cast<uint64_t>(value) << 16) |
        (static_cast<uint64_t>(index) << 32) |
        (static_cast<uint64_t>(length) << 48);

    const uint32_t trt = length == 0 ? 0u : (direction_in ? 3u : 2u);
    Trb setup_trb{};
    setup_trb.parameter = setup;
    setup_trb.status = 8;
    setup_trb.control = trb_type(TRB_SETUP_STAGE) | kTrbIdt | (trt << 16);
    (void)ring_enqueue(dev.control_ring, setup_trb);

    uint64_t data_phys = 0;
    if (length != 0) {
        Trb data_trb{};
        data_trb.parameter = dev.control_buffer_phys;
        data_trb.status = length;
        data_trb.control = trb_type(TRB_DATA_STAGE) | kTrbChain |
                           (direction_in ? (1u << 16) | kTrbIsp : 0);
        if (!direction_in) {
            libk::copy_n(libk::as_bytes(dev.control_buffer, kControlBufferBytes),
                         libk::as_bytes(buffer, length), length);
        }
        data_phys = ring_enqueue(dev.control_ring, data_trb);
    }

    Trb status_trb{};
    status_trb.parameter = 0;
    status_trb.status = 0;
    status_trb.control = trb_type(TRB_STATUS_STAGE) | kTrbIoc |
                         ((!direction_in || length == 0) ? (1u << 16) : 0);
    const auto status_phys = ring_enqueue(dev.control_ring, status_trb);

    barrier();
    doorbell_write(dev.slot_id, 1);

    uint32_t code = 0;
    uint32_t actual = 0;
    if (!wait_transfer(dev.slot_id, 1, status_phys, data_phys, length, &actual, &code)) return false;
    (void)dev.control_ring.retire(descriptors);
    if (direction_in && actual) {
        if (!libk::copy_n(libk::as_bytes(buffer, length),
                         libk::as_bytes(dev.control_buffer, kControlBufferBytes), actual)) return false;
    }
    (void)code;
    return actual == length;
}

static bool get_descriptor(UsbDevice& dev, uint8_t type, uint8_t index,
                           uint8_t* buffer, uint16_t length) {
    return usb_control(dev, 0x80, 6,
                       static_cast<uint16_t>((static_cast<uint16_t>(type) << 8) | index),
                       0, buffer, length, true);
}

static bool set_configuration(UsbDevice& dev, uint8_t value) {
    return usb_control(dev, 0x00, 9, value, 0, nullptr, 0, false);
}

static bool hid_set_protocol(UsbDevice& dev, uint8_t iface, uint8_t protocol) {
    return usb_control(dev, 0x21, 0x0B, protocol, iface, nullptr, 0, false);
}

static bool hid_set_idle(UsbDevice& dev, uint8_t iface) {
    return usb_control(dev, 0x21, 0x0A, 0, iface, nullptr, 0, false);
}

static bool parse_keyboard_config(UsbDevice& dev, uint16_t total_len) {
    // Validate the entire tree before selecting the legacy keyboard adapter.
    // Capacity derives from the bounded descriptor blob, not a device count.
    midix::usb::InterfaceDescriptor interfaces[kControlBufferBytes / 9]{};
    midix::usb::EndpointDescriptor endpoints[kControlBufferBytes / 7]{};
    midix::usb::Configuration config{interfaces, endpoints, kControlBufferBytes / 9,
                                    kControlBufferBytes / 7, 0, 0, 0, 0, 0, 0};
    MxUsbSpeed speed{};
    if (!midix::usb::xhci::standard_speed(dev.speed, &speed)) return false;
    if (!midix::usb::parse_configuration(libk::as_bytes(dev.control_buffer, total_len),
                                       speed, &config)) return false;
    for (uint32_t i = 0; i < config.endpoint_count; ++i) {
        const auto& endpoint = endpoints[i];
        const auto& interface = interfaces[endpoint.interface_index];
        if (interface.alternate != 0 || interface.interface_class != 3 || interface.subclass != 1 ||
            interface.protocol != 1 || endpoint.type != MxUsbTransferType::Interrupt ||
            !(endpoint.address & 0x80u) || endpoint.max_packet < sizeof(UsbBootKeyboardReport)) continue;
        dev.config_value = config.value;
        dev.interface_number = interface.number;
        dev.endpoint_address = endpoint.address;
        dev.endpoint_id = static_cast<uint8_t>((endpoint.address & 15u) * 2u + 1u);
        dev.endpoint_mps = endpoint.max_packet;
        dev.interval = endpoint.interval;
        return true;
    }
    return false;
}

static uint32_t endpoint_interval(uint8_t speed, uint8_t b_interval) {
    uint32_t interval = 0;
    MxUsbSpeed logical_speed{};
    if (midix::usb::xhci::standard_speed(speed, &logical_speed))
        (void)midix::usb::xhci::interrupt_interval(logical_speed, b_interval, &interval);
    return interval;
}

static bool configure_keyboard_endpoint(UsbDevice& dev) {
    if (!ring_init(g_keyboard.interrupt_ring, 64, true)) return false;
    g_keyboard.report = static_cast<UsbBootKeyboardReport*>(
        g_dma_arena.alloc_zeroed(sizeof(UsbBootKeyboardReport), 64));
    if (!g_keyboard.report) return false;
    g_keyboard.report_phys = dma_phys(g_keyboard.report);

    const uint64_t context_bytes = static_cast<uint64_t>(33u) * g_ctx_stride;
    libk::zero(dev.input_context, context_bytes);

    auto* icc = reinterpret_cast<uint32_t*>(dev.input_context);
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << dev.endpoint_id);

    uint32_t* slot = input_context_dwords(dev, 0);
    libk::copy_exact(libk::as_bytes(slot, g_ctx_stride),
                     libk::as_bytes(output_context_dwords(dev, 0), g_ctx_stride));
    slot[0] = (slot[0] & ~(0x1Fu << 27)) |
              (static_cast<uint32_t>(dev.endpoint_id) << 27);

    uint32_t* ep = input_context_dwords(dev, dev.endpoint_id);
    ep[0] = endpoint_interval(dev.speed, dev.interval) << 16;
    ep[1] = (3u << 1) | (7u << 3) |
            (static_cast<uint32_t>(dev.endpoint_mps) << 16);
    const uint64_t dequeue = g_keyboard.interrupt_ring.phys | 1ull;
    ep[2] = static_cast<uint32_t>(dequeue);
    ep[3] = static_cast<uint32_t>(dequeue >> 32);
    ep[4] = static_cast<uint32_t>(dev.endpoint_mps) |
            (static_cast<uint32_t>(dev.endpoint_mps) << 16);
    barrier();

    Trb event{};
    Trb configure{};
    configure.parameter = dev.input_context_phys;
    configure.control = trb_type(TRB_CONFIGURE_ENDPOINT) |
                        (static_cast<uint32_t>(dev.slot_id) << 24);
    if (!issue_command(configure, &event)) return false;

    g_keyboard.slot_id = dev.slot_id;
    g_keyboard.endpoint_id = dev.endpoint_id;
    return true;
}

static uint32_t doorbell_target_for_endpoint(uint8_t endpoint_id) {
    // xHCI device doorbells use the endpoint context index directly:
    // 1 = default control, 2 = EP1 OUT, 3 = EP1 IN, and so on.
    return endpoint_id == 0 ? 1u : static_cast<uint32_t>(endpoint_id);
}

static void submit_keyboard_transfer() {
    if (!g_keyboard.ready) return;
    libk::zero(g_keyboard.report, sizeof(UsbBootKeyboardReport));
    Trb trb{};
    trb.parameter = g_keyboard.report_phys;
    trb.status = sizeof(UsbBootKeyboardReport);
    trb.control = trb_type(TRB_NORMAL) | kTrbIoc | kTrbIsp;
    (void)ring_enqueue(g_keyboard.interrupt_ring, trb);
    barrier();
    doorbell_write(g_keyboard.slot_id,
                   doorbell_target_for_endpoint(g_keyboard.endpoint_id));
}

static bool send_keyboard_report(const UsbBootKeyboardReport* report) {
    return report && g_hid.try_send(*report);
}

static bool enumerate_port(uint8_t port) {
    UsbDevice dev{};
    dev.port = port;
    auto checked = [](bool success, uint32_t stage) {
        if (!success) mochi::log_counter("xhci.enumeration.failure", stage);
        return success;
    };
    if (!checked(reset_port(port, &dev.speed), 1)) return false;
    if (!checked(device_alloc(dev), 2)) return false;
    if (!checked(address_device(dev), 3)) return false;

    if (!checked(get_descriptor(dev, 1, 0, dev.control_buffer, 18), 4)) return false;
    if (!checked(get_descriptor(dev, 2, 0, dev.control_buffer, 9), 5)) return false;
    uint16_t total_len = libk::load_le<uint16_t>(dev.control_buffer + 2);
    if (total_len < 9) return false;
    if (total_len > kControlBufferBytes) return false;
    if (!checked(get_descriptor(dev, 2, 0, dev.control_buffer, total_len), 6)) return false;
    if (!checked(parse_keyboard_config(dev, total_len), 7)) return false;

    if (!checked(configure_keyboard_endpoint(dev), 8)) return false;
    if (!checked(set_configuration(dev, dev.config_value), 9)) return false;
    if (!checked(hid_set_protocol(dev, dev.interface_number, 0), 10)) return false;
    (void)hid_set_idle(dev, dev.interface_number);

    g_keyboard.ready = true;
    submit_keyboard_transfer();
    return true;
}

static bool enumerate_keyboard() {
    for (uint32_t port = 1; port <= g_num_ports; ++port) {
        if (enumerate_port(static_cast<uint8_t>(port))) return true;
    }
    return false;
}

static void service_events() {
    Trb event{};
    bool any = false;
    while (g_deferred_events.pop(&event) || event_poll(&event)) {
        any = true;
        if (event_type(event) == TRB_TRANSFER_EVENT) {
            const uint32_t code = completion_code(event);
            if (!g_keyboard.ready ||
                event_slot(event) != g_keyboard.slot_id ||
                event_endpoint(event) != g_keyboard.endpoint_id) {
                continue;
            }
            if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
                barrier();
                const auto residual = event.status & 0xffffffu;
                if (residual == 0) g_keyboard.report_pending = true;
            }
            (void)g_keyboard.interrupt_ring.retire(1);
            if (code != CC_SUCCESS && code != CC_SHORT_PACKET) g_keyboard.ready = false;
            else if (!g_keyboard.report_pending) submit_keyboard_transfer();
        }
    }
    if (g_keyboard.report_pending && send_keyboard_report(g_keyboard.report)) {
        g_keyboard.report_pending = false;
        if (g_keyboard.ready) submit_keyboard_transfer();
    }
    if (g_event_fault) {
        // Do not recycle DMA or continue after losing event ownership.
        g_keyboard.ready = false;
        op_write(kXhciUsbCmd, op_read(kXhciUsbCmd) & ~kCmdRun);
    }
    if (!any) mx_yield();
}

extern "C" [[noreturn]] void _start() {
    MxBootInfo* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.usb.xhci");

    const mochi::Ring hid = mochi::boot::map_ring(
        boot, {"xhci.hid.ring", kVaHidRing, "xhci.hid.doorbell"});
    static_cast<mochi::Ring&>(g_hid) = hid;

    mx_handle_t xhci_mmio_cap = mochi::boot::handle(boot, "xhci.mmio");
    if (xhci_mmio_cap == MX_NULL_HANDLE) {
        mochi::log_raw_marker("drv.usb.xhci: no xhci.mmio in BootInfo\n");
        mochi::boot::ready(boot);
        while (true) { mx_yield(); }
    }
    MxCapInfo mmio_info{};
    if (mx_cap_info(xhci_mmio_cap, &mmio_info) != MX_OK || mmio_info.size < 0x20 ||
        mx_map_mmio(xhci_mmio_cap, kVaXhciMmio,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
        mochi::log_raw_marker("drv.usb.xhci: xhci.mmio map failed\n");
        while (true) { mx_yield(); }
    }
    g_mmio = {reinterpret_cast<volatile uint8_t*>(kVaXhciMmio), mmio_info.size};

    mx_handle_t dma_cap = mochi::boot::handle(boot, "xhci.dma");
    MxCapInfo dma_info{};
    if (dma_cap == MX_NULL_HANDLE ||
        mx_cap_info(dma_cap, &dma_info) != MX_OK ||
        dma_info.attr != MX_ARENA_MEMORY_NONCACHEABLE ||
        mx_map_page(dma_cap, kVaXhciDma,
                    MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK) {
        mochi::log_raw_marker("drv.usb.xhci: no dma arena\n");
        while (true) { mx_yield(); }
    }
    g_dma_base = reinterpret_cast<uint8_t*>(kVaXhciDma);
    g_dma_phys = dma_info.base;
    g_dma_size = dma_info.size;
    g_dma_arena = mochi::Arena(g_dma_base, g_dma_size);

    mx_handle_t state_cap = MX_NULL_HANDLE;
    if (mx_arena_create(4096, 0, &state_cap) != MX_OK ||
        mx_map_page(state_cap, kVaXhciState, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK ||
        !g_deferred_events.init(reinterpret_cast<Trb*>(kVaXhciState), 4096 / sizeof(Trb))) {
        mochi::log_raw_marker("drv.usb.xhci: state arena allocation failed\n");
        while (true) mx_yield();
    }

    // V0 deliberately polls the xHCI event ring. Keep the IRQ cap out of use
    // until kernel IRQ routing and EOI/ack semantics are wired end to end.
    (void)mochi::boot::handle(boot, "xhci.irq");

    g_cap_len = cap_read(kXhciCapLength) & 0xFFu;
    const uint32_t hcs1 = cap_read(kXhciHcsParams1);
    const uint32_t hcc1 = cap_read(kXhciHccParams1);
    g_max_slots = hcs1 & 0xFFu;
    g_num_ports = (hcs1 >> 24) & 0xFFu;
    g_ctx_stride = (hcc1 & (1u << 2)) ? 64u : 32u;
    g_dboff = cap_read(kXhciDbOff) & ~0x3u;
    g_rtsoff = cap_read(kXhciRtsoff) & ~0x1Fu;

    const uint64_t port_end = static_cast<uint64_t>(g_cap_len) + kXhciPorts +
                              static_cast<uint64_t>(g_num_ports) * 0x10;
    const uint64_t doorbell_end = static_cast<uint64_t>(g_dboff) + (g_max_slots + 1ull) * 4;
    if (g_event_fault || g_cap_len < 0x20 || (g_cap_len & 3) || !g_max_slots || !g_num_ports ||
        port_end > g_mmio.size || doorbell_end > g_mmio.size ||
        static_cast<uint64_t>(g_rtsoff) + 0x40 > g_mmio.size ||
        (!(hcc1 & 1) && (g_dma_phys > 0xffffffffull ||
         !g_dma_size || g_dma_size - 1 > 0xffffffffull - g_dma_phys))) {
        mochi::log_raw_marker("drv.usb.xhci: invalid controller geometry or DMA width\n");
        mochi::boot::ready(boot);
        while (true) mx_yield();
    }

    if (!xhci_reset()) {
        mochi::log_raw_marker("drv.usb.xhci: controller reset timeout\n");
        while (true) { mx_yield(); }
    }
    if (!xhci_dma_init() || !xhci_program_and_start()) {
        mochi::log_raw_marker("drv.usb.xhci: controller init failed\n");
        while (true) { mx_yield(); }
    }

    mochi::log_raw_marker("drv.usb.xhci: controller ready\n");

    if (enumerate_keyboard()) {
        mochi::log_raw_marker("drv.usb.xhci: keyboard ready\n");
    } else {
        mochi::log_raw_marker("drv.usb.xhci: keyboard not found\n");
    }

    mochi::boot::ready(boot);
    while (true) {
        service_events();
    }
}

// Controller mechanisms only: ports, contexts, TRBs, DMA and completions.
// Enumeration and interface policy are in io.usb; class drivers own reports.
#include "mochi/syscall.h"
#include "mochi/arena.h"
#include "mochi/boot.h"
#include "mochi/boot_log.h"
#include "mochi/log.h"
#include "mochi/message.h"
#include "mochi/usb/link.h"
#include "mochi/usb/transfer_engine.h"
#include "mochi/usb/xhci_exec.h"
#include "midix/abi/protocols/usb_controller.h"
#include "midix/usb/xhci_events.h"
#include "libk/mmio.h"
#include "libk/dma.h"
#include "libk/cpu.h"
#include "mochi/driver.h"
#include "libk/endian.h"
#include "libk/slot_pool.h"
#include "mochi/va_layout.h"
using namespace mochi::va::xhci;
constexpr uint32_t kControlBufferBytes = 4096;
constexpr uint32_t kStateBytes = 64 * 1024;
constexpr uint32_t kDevices = 8, kEndpoints = 32, kTransfers = 16;
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
    uint8_t port, slot_id, speed;
    MxUsbToken ep0;
    uint8_t* input_context;
    uint64_t input_context_phys;
    uint8_t* output_context;
    uint64_t output_context_phys;
};
struct EndpointCold {
    MxUsbToken device, engine;
    XhciRing ring;
    uint8_t address, context_id;
    uint16_t max_packet;
    MxUsbTransferType type;
};
struct TransferCold {
    MxUsbToken public_endpoint;
    uint64_t request_id;
    uint32_t shared_offset;
    uint64_t deadline;
    bool input;
};
// Dense hot columns are scanned for completion/backpressure; context/ring
// descriptors and setup state are cold and indexed by generational tokens.
struct Runtime {
    UsbDevice devices[kDevices];
    EndpointCold endpoints[kEndpoints];
    MxUsbToken endpoint_active[kEndpoints];
    MxUsbToken transfers[kTransfers];
    uint8_t payload_copied[kTransfers];
    TransferCold transfer_cold[kTransfers];
    MxUsbToken dma_buffers[kTransfers];
    uint8_t* dma_payload[kTransfers];
    libk::SlotMetadata device_slots[kDevices], endpoint_slots[kEndpoints];
    uint8_t port_dirty[256];
    uint64_t port_sequence[256];
    mochi::SpscEndpoint link;
    libk::bytes shared;
    MxUsbControllerReply reply;
    bool reply_pending;
    uint64_t next_deadline;
};
static Runtime* g;
static libk::SlotPool g_device_pool, g_endpoint_pool;
static mochi::usb::TransferEngine g_engine;
static mochi::usb::ExecTable g_exec;
static mochi::usb::SessionScope g_scope{1, 1, 1};
static mochi::Arena g_state_arena, g_dma_arena;
static libk::Mmio g_mmio{};
static uint32_t g_cap_len, g_dboff, g_rtsoff, g_max_slots, g_num_ports, g_ctx_stride = 32;
static uint8_t* g_dma_base;
static uint64_t g_dma_phys, g_dma_size;
static uint64_t* g_dcbaa;
static XhciRing g_command_ring;
static Trb* g_event_ring;
static uint64_t g_event_ring_phys;
static uint32_t g_event_count, g_event_index, g_event_cycle = 1;
static ErstEntry* g_erst;
static uint64_t g_erst_phys;
static midix::usb::xhci::EventBacklog g_deferred_events;
static bool g_event_fault;
static UsbDevice* device(MxUsbToken token) {
    const auto i = g_device_pool.index_of(token);
    return i == libk::kNoSlot ? nullptr : &g->devices[i];
}
static EndpointCold* endpoint(MxUsbToken token) {
    const auto i = g_endpoint_pool.index_of(token);
    return i == libk::kNoSlot ? nullptr : &g->endpoints[i];
}
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


static bool route_event(const Trb& event) {
    if (event_type(event) == TRB_TRANSFER_EVENT) return g_exec.on_event(event, g_engine);
    if (event_type(event) != TRB_PORT_STATUS_CHANGE_EVENT) return false;
    const auto port = static_cast<uint8_t>(event.parameter >> 24);
    if (!port || port > g_num_ports) return false;
    const auto status = port_read(port);
    if (status & (1u << 17)) { ++g->port_sequence[port]; g->port_dirty[port] = 1; }
    port_clear_changes(port);
    return true;
}
static uint32_t pump_events() {
    uint32_t count = 0;
    Trb event{};
    while (g_deferred_events.pop(&event)) { (void)route_event(event); ++count; }
    while (event_poll(&event)) {
        if (!route_event(event) && event_type(event) != TRB_TRANSFER_EVENT && !g_deferred_events.push(event)) g_event_fault = true;
        ++count;
    }
    return count;
}
static bool wait_command(uint64_t command_phys, Trb* out_event) {
    for (uint32_t spin = 0; spin < 500000; ++spin) {
        Trb event{};
        // Deferred items can only be stale copies (the command was issued after
        // the last drain), so each is considered once and dropped when nothing
        // claims it.
        while (g_deferred_events.pop(&event)) (void)route_event(event);
        while (event_poll(&event)) {
            if (event_type(event) == TRB_COMMAND_COMPLETION_EVENT &&
                event.parameter == command_phys) {
                if (out_event) *out_event = event;
                return completion_code(event) == CC_SUCCESS;
            }
            if (!route_event(event) && event_type(event) != TRB_TRANSFER_EVENT) {
                if (!g_deferred_events.push(event)) { g_event_fault = true; return false; }
            }
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

static bool xhci_reset() {
    if (!(op_read(kXhciUsbSts) & kStsHcHalted)) {
        op_write(kXhciUsbCmd, op_read(kXhciUsbCmd) & ~kCmdRun);
        uint32_t attempts = 100000;
        while (!(op_read(kXhciUsbSts) & kStsHcHalted) && --attempts) {
            libk::cpu::relax();
        }
        if (!attempts) return false;
    }

    uint32_t attempts = 100000;
#ifndef ANKO_XHCI_NORESET
    // T210 keeps Hekate's FALCON firmware; other controllers reset normally.
    op_write(kXhciUsbCmd, op_read(kXhciUsbCmd) | kCmdHcReset);
    while ((op_read(kXhciUsbCmd) & kCmdHcReset) && --attempts) {
        libk::cpu::relax();
    }
    if (!attempts) return false;
    attempts = 100000;
#endif
    while ((op_read(kXhciUsbSts) & kStsNotReady) && --attempts) {
        libk::cpu::relax();
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
        libk::cpu::relax();
    }
    return attempts > 0;
}

static bool reset_port(uint8_t port, uint8_t* out_speed) {
    uint32_t s = port_read(port);
    if ((s & kPortCcs) == 0) return false;

    if ((s & kPortPp) == 0) {
        port_write(port, s | kPortPp);
        for (uint32_t i = 0; i < 10000; ++i) libk::cpu::relax();
        s = port_read(port);
    }

    port_clear_changes(port);
    s = port_read(port);
    port_write(port, s | kPortPr);

    uint32_t attempts = 250000;
    while ((port_read(port) & kPortPr) && --attempts) {
        libk::cpu::relax();
    }
    if (!attempts) return false;

    attempts = 250000;
    while (((s = port_read(port)) & kPortPed) == 0 && --attempts) {
        libk::cpu::relax();
    }
    if (!attempts) return false;

    port_clear_changes(port);
    if (out_speed) *out_speed = static_cast<uint8_t>((s >> 10) & 0xFu);
    return true;
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
    const uint64_t dequeue = endpoint(dev.ep0)->ring.phys | 1ull;
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
    prepare_address_context(dev, endpoint(dev.ep0)->max_packet);

    Trb address{};
    address.parameter = dev.input_context_phys;
    address.control = trb_type(TRB_ADDRESS_DEVICE) |
                      (static_cast<uint32_t>(dev.slot_id) << 24);
    return issue_command(address, &event);
}


static bool endpoint_install(MxUsbToken device_token, const midix::usb::EndpointDescriptor& desc,
                             MxUsbSpeed speed, bool control, MxUsbToken* out) {
    auto token = g_endpoint_pool.acquire();
    if (!token) return false;
    auto* ep = endpoint(token);
    ep->device = device_token;
    ep->address = desc.address; ep->max_packet = desc.max_packet;
    ep->context_id = control ? 1 : static_cast<uint8_t>(((desc.address & 15u) * 2) + ((desc.address >> 7) & 1u));
    ep->type = desc.type;
    if (!ep->ring.trbs && !ring_init(ep->ring, 64, true)) {
        g_endpoint_pool.release(token); return false;
    }
    if (ep->ring.trbs) ep->ring.init(ep->ring.trbs, ep->ring.phys, ep->ring.count);
    const auto rc = g_engine.install_endpoint({g_scope, device_token, desc, speed, 0,
        control ? mochi::usb::ControlPolicy::Bus : mochi::usb::ControlPolicy::None,
        control ? device_token : 0}, &ep->engine);
    if (rc != MX_OK) { g_endpoint_pool.release(token); return false; }
    *out = token;
    return true;
}
static mx_status_t add_device(uint32_t port, MxUsbControllerReply& reply) {
    if (!port || port > g_num_ports) return MX_ERR_INVALID;
    for (uint32_t i = 0; i < kDevices; ++i)
        if (g_device_pool.token_at(i) && g->devices[i].port == port) return MX_ERR_BUSY;
    const auto token = g_device_pool.acquire();
    if (!token) return MX_ERR_WOULD_BLOCK;
    auto& dev = *device(token);
    dev.port = static_cast<uint8_t>(port); dev.slot_id = 0;
    if (!reset_port(dev.port, &dev.speed)) { g_device_pool.release(token); return MX_ERR_FAULT; }
    MxUsbSpeed speed{};
    if (!midix::usb::xhci::standard_speed(dev.speed, &speed)) { g_device_pool.release(token); return MX_ERR_NOT_IMPLEMENTED; }
    if (!dev.input_context) {
        dev.input_context = static_cast<uint8_t*>(g_dma_arena.alloc_zeroed(33u * g_ctx_stride, 64));
        dev.output_context = static_cast<uint8_t*>(g_dma_arena.alloc_zeroed(32u * g_ctx_stride, 64));
        if (!dev.input_context || !dev.output_context) return MX_ERR_OVERFLOW;
        dev.input_context_phys = dma_phys(dev.input_context);
        dev.output_context_phys = dma_phys(dev.output_context);
    }
    midix::usb::EndpointDescriptor desc{};
    desc.type = MxUsbTransferType::Control;
    desc.max_packet = speed == MxUsbSpeed::Super ? 512 : speed == MxUsbSpeed::High ? 64 : 8;
    if (!endpoint_install(token, desc, speed, true, &dev.ep0)) return MX_ERR_OVERFLOW;
    if (!address_device(dev)) return MX_ERR_FAULT;
    reply.device = token; reply.endpoint = dev.ep0; reply.speed = speed;
    return MX_OK;
}
static bool configure_endpoint(const MxUsbEndpointConfig& cfg, MxUsbToken* out) {
    auto* dev = device(cfg.device);
    if (!dev) return false;
    MxUsbSpeed speed{};
    if (!midix::usb::xhci::standard_speed(dev->speed, &speed)) return false;
    midix::usb::EndpointDescriptor desc{};
    desc.address = cfg.address; desc.type = cfg.type; desc.max_packet = cfg.max_packet;
    desc.interval = cfg.interval; desc.transactions = 1; desc.max_burst = cfg.max_burst;
    desc.companion_attributes = cfg.mult; desc.has_companion = speed >= MxUsbSpeed::Super; desc.bytes_per_interval = cfg.max_esit_payload;
    if ((desc.type != MxUsbTransferType::Bulk && desc.type != MxUsbTransferType::Interrupt) ||
        !midix::usb::endpoint_valid(desc, speed)) return false;
    for (uint32_t i = 0; i < kEndpoints; ++i)
        if (g_endpoint_pool.token_at(i) && g->endpoints[i].device == cfg.device &&
            g->endpoints[i].address == cfg.address) return false;
    if (!endpoint_install(cfg.device, desc, speed, false, out)) return false;
    auto& ep = *endpoint(*out);
    libk::zero(dev->input_context, 33u * g_ctx_stride);
    auto* icc = reinterpret_cast<uint32_t*>(dev->input_context);
    icc[1] = (1u << 0) | (1u << ep.context_id);
    auto* slot = input_context_dwords(*dev, 0);
    libk::copy_exact(libk::as_bytes(slot, g_ctx_stride), libk::as_bytes(output_context_dwords(*dev, 0), g_ctx_stride));
    const uint32_t old_entries = (slot[0] >> 27) & 31;
    if (ep.context_id > old_entries) slot[0] = (slot[0] & ~(31u << 27)) | (static_cast<uint32_t>(ep.context_id) << 27);
    auto* context = input_context_dwords(*dev, ep.context_id);
    uint32_t interval = 0;
    if (desc.type == MxUsbTransferType::Interrupt &&
        !midix::usb::xhci::interrupt_interval(speed, desc.interval, &interval)) return false;
    context[0] = (static_cast<uint32_t>(interval) << 16) | (static_cast<uint32_t>(desc.companion_attributes) << 8);
    const uint32_t type = desc.type == MxUsbTransferType::Bulk ? ((desc.address & 128) ? 6 : 2) : ((desc.address & 128) ? 7 : 3);
    context[1] = (3u << 1) | (type << 3) | (static_cast<uint32_t>(desc.max_burst) << 8) |
        (static_cast<uint32_t>(desc.max_packet) << 16);
    const auto dequeue = ep.ring.phys | 1;
    context[2] = static_cast<uint32_t>(dequeue); context[3] = static_cast<uint32_t>(dequeue >> 32);
    context[4] = desc.max_packet | (static_cast<uint32_t>(desc.bytes_per_interval ? desc.bytes_per_interval : desc.max_packet) << 16);
    barrier();
    Trb command{}; command.parameter = dev->input_context_phys;
    command.control = trb_type(TRB_CONFIGURE_ENDPOINT) | (static_cast<uint32_t>(dev->slot_id) << 24);
    return issue_command(command, nullptr);
}
static void publish_error(uint64_t request, MxUsbToken ep, mx_status_t error) {
    g->reply = {request, 0, ep, error, 0, {}, {}};
    g->reply_pending = true;
}
static void submit_transfer(const MxUsbSubmit& wire, const MxUsbControlSubmit* control) {
    auto* ep = endpoint(wire.endpoint);
    auto* dev = ep ? device(ep->device) : nullptr;
    const auto ei = g_endpoint_pool.index_of(wire.endpoint);
    const uint32_t length = control ? control->setup.length : wire.length;
    const auto offset = control ? control->offset : wire.offset;
    const bool input = control ? (control->setup.request_type & 128u) != 0 : ep && (ep->address & 128u);
    if (g_event_fault || !ep || !dev || (control && ep->type != MxUsbTransferType::Control) ||
        (!control && ep->type == MxUsbTransferType::Control) || length > kControlBufferBytes ||
        (length && wire.buffer != 1) || offset > g->shared.size || length > g->shared.size - offset ||
        (control && control->interface != ep->device)) {
        publish_error(wire.request_id, wire.endpoint, MX_ERR_INVALID); return;
    }
    if (g->endpoint_active[ei]) { publish_error(wire.request_id, wire.endpoint, MX_ERR_BUSY); return; }
    uint32_t ti = 0;
    for (; ti < kTransfers && g->transfers[ti]; ++ti) {}
    if (ti == kTransfers) { publish_error(wire.request_id, wire.endpoint, MX_ERR_WOULD_BLOCK); return; }
    const uint32_t descriptors = control ? (length ? 3 : 2) : 1;
    if (ep->ring.available() < descriptors) { publish_error(wire.request_id, wire.endpoint, MX_ERR_WOULD_BLOCK); return; }
    if (!input && length) libk::copy_n(libk::as_bytes(g->dma_payload[ti], kControlBufferBytes),
        libk::as_bytes(g->shared.data + offset, length), length);
    MxUsbToken token = 0;
    mx_status_t rc;
    if (control) {
        auto request = *control; request.buffer = length ? g->dma_buffers[ti] : 0; request.offset = 0;
        rc = g_engine.submit_control(g_scope, ep->engine, request, request.deadline_ns ? mx_clock_ns() : 0, &token);
    } else {
        auto request = wire; request.endpoint = ep->engine; request.buffer = length ? g->dma_buffers[ti] : 0; request.offset = 0;
        rc = g_engine.submit(g_scope, request, request.deadline_ns ? mx_clock_ns() : 0, &token);
    }
    if (rc != MX_OK) { publish_error(wire.request_id, wire.endpoint, rc); return; }
    mochi::usb::PreparedTransfer prepared{};
    if (g_engine.start(token, &prepared) != MX_OK) { g_event_fault = true; return; }
    uint64_t head = 0, data = 0, last = 0;
    if (control) {
        uint64_t setup = 0;
        libk::copy_exact(libk::as_bytes(&setup, 8), libk::as_bytes(&control->setup, 8));
        head = ep->ring.enqueue({setup, 8, trb_type(TRB_SETUP_STAGE) | kTrbIdt |
            ((length ? (input ? 3u : 2u) : 0u) << 16)});
        if (length) data = ep->ring.enqueue({prepared.device_address, length,
            trb_type(TRB_DATA_STAGE) | kTrbChain | (input ? (1u << 16) | kTrbIsp : 0)});
        last = ep->ring.enqueue({0, 0, trb_type(TRB_STATUS_STAGE) | kTrbIoc |
            ((input && length) ? 0 : (1u << 16))});
    } else {
        head = last = ep->ring.enqueue({prepared.device_address, length, trb_type(TRB_NORMAL) | kTrbIoc | kTrbIsp});
    }
    if (g_exec.record(token, wire.request_id, dev->slot_id, ep->context_id, &ep->ring,
        descriptors, head, last, data, length) != MX_OK) { g_event_fault = true; return; }
    g->transfers[ti] = token; g->payload_copied[ti] = 0;
    g->transfer_cold[ti] = {wire.endpoint, wire.request_id, offset, wire.deadline_ns, input};
    if (wire.deadline_ns && (!g->next_deadline || wire.deadline_ns < g->next_deadline)) g->next_deadline = wire.deadline_ns;
    g->endpoint_active[ei] = token;
    barrier(); doorbell_write(dev->slot_id, ep->context_id);
}
static bool stop_endpoint(MxUsbToken ep_token, MxUsbStatus reason) {
    auto* ep = endpoint(ep_token); auto* dev = ep ? device(ep->device) : nullptr;
    if (!ep || !dev) return false;
    const auto ei = g_endpoint_pool.index_of(ep_token);
    const auto token = g->endpoint_active[ei];
    if (token) (void)g_engine.stop(token, reason);
    libk::dma::consume();
    const auto state = output_context_dwords(*dev, ep->context_id)[0] & 7u;
    if (state == 1) {
        Trb command{}; command.control = trb_type(15) | (static_cast<uint32_t>(ep->context_id) << 16) |
            (static_cast<uint32_t>(dev->slot_id) << 24);
        if (!issue_command(command, nullptr)) return false;
    } else if (state == 2) {
        Trb reset{}; reset.control = trb_type(14) | (static_cast<uint32_t>(ep->context_id) << 16) |
            (static_cast<uint32_t>(dev->slot_id) << 24);
        if (!issue_command(reset, nullptr)) return false;
    } else if (state != 3) return false;
    (void)pump_events();
    if (token) {
        auto* entry = g_exec.find(token); if (!entry) return false;
        if (g_engine.state(token) == mochi::usb::TransferState::Stopping && g_engine.stopped(token) != MX_OK) return false;
        if (!(entry->flags & mochi::usb::kExecRingRetired)) {
            if (!entry->ring->retire(entry->descriptors)) return false;
            entry->flags |= mochi::usb::kExecRingRetired;
        }
        entry->flags |= mochi::usb::kExecDone;
    }
    Trb dequeue{};
    dequeue.parameter = ep->ring.phys + static_cast<uint64_t>(ep->ring.index) * sizeof(Trb) + ep->ring.cycle;
    dequeue.control = trb_type(16) | (static_cast<uint32_t>(ep->context_id) << 16) |
        (static_cast<uint32_t>(dev->slot_id) << 24);
    return issue_command(dequeue, nullptr);
}
static bool close_endpoint(MxUsbToken ep_token) {
    auto* ep = endpoint(ep_token); auto* dev = ep ? device(ep->device) : nullptr;
    if (!ep || !dev || ep->context_id == 1 || !stop_endpoint(ep_token, MxUsbStatus::Disconnected)) return false;
    libk::zero(dev->input_context, 33u * g_ctx_stride);
    auto* icc = reinterpret_cast<uint32_t*>(dev->input_context);
    icc[0] = 1u << ep->context_id; icc[1] = 1;
    libk::copy_exact(libk::as_bytes(input_context_dwords(*dev, 0), g_ctx_stride),
        libk::as_bytes(output_context_dwords(*dev, 0), g_ctx_stride));
    Trb command{}; command.parameter = dev->input_context_phys;
    command.control = trb_type(TRB_CONFIGURE_ENDPOINT) | (static_cast<uint32_t>(dev->slot_id) << 24);
    barrier(); if (!issue_command(command, nullptr)) return false;
    (void)pump_events();
    (void)g_engine.revoke_endpoint(g_scope, ep->engine);
    g->endpoint_active[g_endpoint_pool.index_of(ep_token)] = 0;
    return g_endpoint_pool.release(ep_token);
}
static bool remove_device(MxUsbToken token) {
    auto* dev = device(token);
    if (!dev) return false;
    if (dev->slot_id) {
        Trb command{}; command.control = trb_type(TRB_DISABLE_SLOT) | (static_cast<uint32_t>(dev->slot_id) << 24);
        if (!issue_command(command, nullptr)) return false;
        (void)pump_events(); // drain old events before any context/ring reuse
    }
    for (uint32_t i = 0; i < kTransfers; ++i) {
        auto* ep = endpoint(g->transfer_cold[i].public_endpoint);
        if (!g->transfers[i] || !ep || ep->device != token) continue;
        auto* entry = g_exec.find(g->transfers[i]);
        if (!entry) return false;
        (void)g_engine.stop(entry->token, MxUsbStatus::Disconnected);
        if (g_engine.state(entry->token) == mochi::usb::TransferState::Stopping) (void)g_engine.stopped(entry->token);
        if (!(entry->flags & mochi::usb::kExecRingRetired)) {
            if (!entry->ring->retire(entry->descriptors)) return false;
            entry->flags |= mochi::usb::kExecRingRetired;
        }
        entry->flags |= mochi::usb::kExecDone;
    }
    for (uint32_t i = 0; i < kEndpoints; ++i) {
        const auto ep_token = g_endpoint_pool.token_at(i);
        if (!ep_token || g->endpoints[i].device != token) continue;
        (void)g_engine.revoke_endpoint(g_scope, g->endpoints[i].engine);
        g->endpoint_active[i] = 0;
        g_endpoint_pool.release(ep_token);
    }
    if (dev->slot_id) { g_dcbaa[dev->slot_id] = 0; barrier(); }
    dev->slot_id = 0;
    g_device_pool.release(token);
    return true;
}
static void cold_request(uint32_t type, const MxUsbControllerRequest& request) {
    g->reply = {request.request_id, 0, 0, MX_OK, 0, {}, {}};
    auto& reply = g->reply;
    if (g_event_fault) reply.status = MX_ERR_FAULT;
    else if (request.reserved) reply.status = MX_ERR_INVALID;
    else if (type == MX_USB_CONTROLLER_NEGOTIATE) {
        for (uint32_t i = 0; i < kDevices; ++i) {
            const auto token = g_device_pool.token_at(i);
            if (token && !remove_device(token)) { reply.status = MX_ERR_FAULT; break; }
        }
        reply.value = g_num_ports;
    }
    else if (type == MX_USB_CONTROLLER_PORT_STATUS) {
        if (!request.value || request.value > g_num_ports) reply.status = MX_ERR_INVALID;
        else { const auto status = port_read(static_cast<uint8_t>(request.value)); reply.value = (status & kPortCcs) ? 1 : 0; }
    } else if (type == MX_USB_CONTROLLER_ADDRESS) reply.status = add_device(request.value, reply);
    else if (type == MX_USB_CONTROLLER_REVOKE_GRANT) reply.status = close_endpoint(request.device) ? MX_OK : MX_ERR_FAULT;
    else if (type == MX_USB_CONTROLLER_REMOVE_DEVICE) reply.status = remove_device(request.device) ? MX_OK : MX_ERR_FAULT;
    else if (type == MX_USB_CONTROLLER_EP0_PACKET) {
        auto* dev = device(request.device);
        const uint32_t packet = request.value;
        if (!dev || (packet != 8 && packet != 16 && packet != 32 && packet != 64 && packet != 512)) reply.status = MX_ERR_INVALID;
        else {
            libk::zero(dev->input_context, 33u * g_ctx_stride);
            reinterpret_cast<uint32_t*>(dev->input_context)[1] = 1u << 1;
            auto* context = input_context_dwords(*dev, 1);
            libk::copy_exact(libk::as_bytes(context, g_ctx_stride), libk::as_bytes(output_context_dwords(*dev, 1), g_ctx_stride));
            context[1] = (context[1] & 0xffffu) | (packet << 16);
            Trb command{}; command.parameter = dev->input_context_phys;
            command.control = trb_type(TRB_EVALUATE_CONTEXT) | (static_cast<uint32_t>(dev->slot_id) << 24);
            barrier();
            if (!issue_command(command, nullptr)) reply.status = MX_ERR_FAULT;
            else {
                auto* ep = endpoint(dev->ep0);
                MxUsbSpeed speed{}; (void)midix::usb::xhci::standard_speed(dev->speed, &speed);
                midix::usb::EndpointDescriptor desc{}; desc.type = MxUsbTransferType::Control;
                desc.max_packet = static_cast<uint16_t>(packet);
                (void)g_engine.revoke_endpoint(g_scope, ep->engine);
                if (g_engine.install_endpoint({g_scope, request.device, desc, speed, 0,
                    mochi::usb::ControlPolicy::Bus, request.device}, &ep->engine) != MX_OK) reply.status = MX_ERR_FAULT;
                ep->max_packet = static_cast<uint16_t>(packet);
            }
        }
    } else reply.status = MX_ERR_NOT_IMPLEMENTED;
    g->reply_pending = true;
}
static bool service() {
    if (g->next_deadline && mx_clock_ns() >= g->next_deadline) {
        g_engine.expire(mx_clock_ns());
        g->next_deadline = 0;
        for (uint32_t i = 0; i < kTransfers; ++i) {
            const auto deadline = g->transfer_cold[i].deadline;
            if (g->transfers[i] && deadline && g_engine.state(g->transfers[i]) == mochi::usb::TransferState::Active &&
                (!g->next_deadline || deadline < g->next_deadline)) g->next_deadline = deadline;
        }
    }
    for (uint32_t i = 0; i < kEndpoints; ++i) {
        const auto token = g_endpoint_pool.token_at(i), transfer = g->endpoint_active[i];
        if (token && transfer && g_engine.state(transfer) == mochi::usb::TransferState::Stopping &&
            !stop_endpoint(token, MxUsbStatus::Timeout)) g_event_fault = true;
    }
    bool progress = pump_events() != 0;
    if (g->reply_pending) {
        if (!g->link.tx().try_push_msg(MX_USB_CONTROLLER_REPLY, g->reply)) return progress;
        g->reply_pending = false; progress = true;
    }
    for (uint32_t i = 0; i < kTransfers; ++i) {
        const auto token = g->transfers[i];
        const auto* done = token ? g_engine.completion(token) : nullptr;
        if (!done) continue;
        const auto& cold = g->transfer_cold[i];
        if (cold.input && done->actual_length && !g->payload_copied[i]) {
            libk::dma::consume();
            if (!libk::copy_n(libk::as_bytes(g->shared.data + cold.shared_offset,
                g->shared.size - cold.shared_offset), libk::as_bytes(g->dma_payload[i], kControlBufferBytes), done->actual_length)) { g_event_fault = true; break; }
        }
        g->payload_copied[i] = 1;
        auto completion = *done; completion.endpoint = cold.public_endpoint;
        if (!g->link.tx().try_push_msg(MX_USB_TRANSFER_COMPLETE, completion)) continue;
        const auto ei = g_endpoint_pool.index_of(cold.public_endpoint);
        if (ei != libk::kNoSlot && g->endpoint_active[ei] == token) g->endpoint_active[ei] = 0;
        auto* entry = g_exec.find(token);
        if (!entry || g_exec.reclaim(*entry, g_engine) != MX_OK) { g_event_fault = true; break; }
        g->transfers[i] = 0; progress = true;
    }
    for (uint32_t port = 1; port <= g_num_ports; ++port) if (g->port_dirty[port]) {
        const auto status = port_read(static_cast<uint8_t>(port));
        MxUsbSpeed speed{}; (void)midix::usb::xhci::standard_speed(static_cast<uint8_t>((status >> 10) & 15), &speed);
        const MxUsbPortEvent event{g->port_sequence[port], static_cast<uint16_t>(port), speed,
            static_cast<uint8_t>((status & kPortCcs) != 0), 0};
        if (!g->link.tx().try_push_msg(MX_USB_CONTROLLER_EVENT, event)) break;
        g->port_dirty[port] = 0; progress = true;
    }
    MxRingSlot message{};
    while (!g->reply_pending && g->link.try_recv(&message)) {
        progress = true;
        if (message.type == MX_USB_TRANSFER_SUBMIT) {
            MxUsbSubmit request{};
            if (mochi::message_get(message, &request, message.type)) submit_transfer(request, nullptr);
        } else if (message.type == MX_USB_TRANSFER_CANCEL) {
            MxUsbCancel request{};
            if (mochi::message_get(message, &request, message.type)) (void)g_engine.cancel(g_scope, request.request_id);
        } else if (message.type == MX_USB_TRANSFER_CONTROL) {
            MxUsbControlSubmit request{};
            if (mochi::message_get(message, &request, message.type)) {
                auto* dev = device(request.interface);
                MxUsbSubmit wire{request.request_id, dev ? dev->ep0 : 0, request.buffer,
                    request.offset, request.setup.length, request.deadline_ns, request.flags, 0};
                submit_transfer(wire, &request);
            }
        } else if (message.type == MX_USB_CONTROLLER_CONFIGURE_ENDPOINT) {
            MxUsbEndpointConfig request{}; MxUsbToken token = 0;
            if (mochi::message_get(message, &request, message.type)) {
                const bool ok = configure_endpoint(request, &token);
                g->reply = {request.request_id, request.device, token, ok ? MX_OK : MX_ERR_INVALID, 0, {}, {}};
                g->reply_pending = true;
            }
        } else {
            MxUsbControllerRequest request{};
            if (mochi::message_get(message, &request, message.type)) cold_request(message.type, request);
        }
    }
    if (g_event_fault) op_write(kXhciUsbCmd, op_read(kXhciUsbCmd) & ~kCmdRun);
    return progress;
}
extern "C" [[noreturn]] void _start() {
    auto* boot = mochi::boot::map(kVaBootInfo);
    (void)mochi::log_map_from_boot(boot, kVaLogRing, "drv.usb.xhci");
    g_state_arena = mochi::usb::make_state(kVaXhciState, kStateBytes);
    g = g_state_arena.alloc<Runtime>(1);
    if (!g) mochi::boot::invalid_boot();
    g->link = mochi::usb::map_link(boot, true, kVaControllerTx, kVaControllerRx);
    g->shared = mochi::usb::map_payload(boot, "usb.controller.payload", kVaControllerPayload);
    if (!g->link.valid() || !g->shared.data) mochi::boot::invalid_boot();
    auto* events = g_state_arena.alloc<Trb>(256);
    auto* records = g_state_arena.alloc<mochi::usb::ExecEntry>(kTransfers);
    if (!g_device_pool.init(g->device_slots, kDevices) || !g_endpoint_pool.init(g->endpoint_slots, kEndpoints) ||
        !g_deferred_events.init(events, 256) || !g_exec.init(records, kTransfers) ||
        !g_engine.init(g_state_arena, {kEndpoints, kTransfers, kTransfers, kControlBufferBytes,
            (1u << 0) | (1u << 2) | (1u << 3)})) mochi::boot::invalid_boot();
    MxCapInfo mmio{}, dma{};
    const auto mmio_cap = mochi::boot::handle(boot, "xhci.mmio");
    if (!mmio_cap || mx_cap_info(mmio_cap, &mmio) != MX_OK ||
        mx_map_mmio(mmio_cap, kVaXhciMmio, MX_RIGHT_READ | MX_RIGHT_WRITE) != MX_OK ||
        !mochi::driver::map_dma(boot, "xhci.dma", kVaXhciDma, &dma)) mochi::boot::invalid_boot();
    g_mmio = {reinterpret_cast<volatile uint8_t*>(kVaXhciMmio), mmio.size};
    g_dma_base = reinterpret_cast<uint8_t*>(kVaXhciDma); g_dma_phys = dma.base; g_dma_size = dma.size;
    g_dma_arena = mochi::Arena(g_dma_base, g_dma_size);
    g_cap_len = cap_read(kXhciCapLength) & 255;
    const auto hcs = cap_read(kXhciHcsParams1), hcc = cap_read(kXhciHccParams1);
    g_max_slots = hcs & 255; g_num_ports = (hcs >> 24) & 255; g_ctx_stride = (hcc & 4) ? 64 : 32;
    g_dboff = cap_read(kXhciDbOff) & ~3u; g_rtsoff = cap_read(kXhciRtsoff) & ~31u;
    if (g_event_fault || g_cap_len < 32 || (g_cap_len & 3) || !g_max_slots || !g_num_ports ||
        static_cast<uint64_t>(g_cap_len) + kXhciPorts + g_num_ports * 16ull > mmio.size ||
        g_dboff + (g_max_slots + 1ull) * 4 > mmio.size || g_rtsoff + 64ull > mmio.size ||
        (!(hcc & 1) && (g_dma_phys > 0xffffffffull || !g_dma_size || g_dma_size - 1 > 0xffffffffull - g_dma_phys)) ||
        !xhci_reset() || !xhci_dma_init()) mochi::boot::invalid_boot();
    for (uint32_t i = 0; i < kTransfers; ++i) {
        g->dma_payload[i] = static_cast<uint8_t*>(g_dma_arena.alloc_zeroed(kControlBufferBytes, kControlBufferBytes));
        if (!g->dma_payload[i] || g_engine.install_buffer({g_scope,
            libk::as_bytes(g->dma_payload[i], kControlBufferBytes), dma_phys(g->dma_payload[i]),
            mochi::usb::BufferRead | mochi::usb::BufferWrite, mochi::usb::CacheMode::NonCacheable, true}, &g->dma_buffers[i]) != MX_OK)
            mochi::boot::invalid_boot();
    }
    if (!xhci_program_and_start()) mochi::boot::invalid_boot();
    mochi::log_raw_marker("drv.usb.xhci: controller ready\n");
    mochi::boot::ready(boot);
    for (;;) {
        if (mochi::driver::run_batch(service)) {
            mx_yield();
            continue;
        }
        // The current PCI path has no usable interrupt binding. Request rings
        // wake immediately; poll hardware and retained TX work on a timer rather
        // than keeping a runnable thread on every CPU idle pass. Actual timeout
        // resolution is the kernel timer tick (currently 10 ms).
        const auto deadline = libk::earlier_deadline(g->next_deadline,
            mx_clock_ns() + mochi::driver::kBackpressureRetryNs);
        mochi::driver::idle(&g->link, 1, g->link.rx_doorbell, deadline,
                            g->reply_pending ? 0u : 1u);
    }
}

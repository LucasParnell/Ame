#include "libk/bits.h"
#include "libk/endian.h"
#include "libk/slot_pool.h"
#include "libk/mmio.h"
#include "midix/hw/pci.h"
#include "midix/abi/protocols/usb_host.h"
#include "midix/abi/protocols/usb_gadget.h"
#include "midix/usb/descriptors.h"
#include "midix/usb/xhci_ring.h"
#include "midix/usb/xhci_events.h"
#include "midix/usb/hid_boot.h"
#include "mochi/usb/transfer_engine.h"
#include "mochi/usb/xhci_exec.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace mochi::usb;
using namespace midix::usb;
static_assert(!__is_constructible(libk::SlotPool, const libk::SlotPool&));
static_assert(!__is_constructible(TransferEngine, const TransferEngine&));
static_assert(!__is_constructible(ExecTable, const ExecTable&));

static void helper_tests() {
    alignas(8) mx_u32_t registers[4]{};
    libk::Mmio mmio{reinterpret_cast<volatile mx_u8_t*>(registers), sizeof(registers)};
    mx_u32_t reg = 0;
    assert(mmio.write32(4, 123) && mmio.read32(4, &reg) && reg == 123);
    assert(!mmio.write32(13, 456) && !mmio.read32(16, &reg) && reg == 123);
    assert(!mmio.write64(4, 123) && mmio.write64(8, 123));
    assert(midix::hw::pci::memory_bar_size(0xffffc000, 0, false) == 0x4000);
    assert(midix::hw::pci::memory_bar_size(0xffffc004, 0xffffffff, true) == 0x4000);
    assert(midix::hw::pci::memory_bar_size(0, 0, true) == 0);
    mx_u8_t bytes[16]{};
    assert(libk::write_le(libk::as_bytes(bytes), mx_u64_t{0x0123456789abcdef}));
    mx_u64_t value = 0;
    assert(libk::read_bits(libk::as_bytes(bytes), 4, 60, &value) && value == 0x0123456789abcde);
    assert(libk::read_bits(libk::as_bytes(bytes), 0, 64, &value) && value == 0x0123456789abcdef);
    value = 17;
    assert(!libk::read_bits(libk::as_bytes(bytes, 8), 1, 64, &value) && value == 17);
    assert(!libk::read_bits(libk::as_bytes(bytes), ~mx_u64_t{0}, 2, &value));
    assert(!libk::write_le(libk::as_bytes(bytes, 1), mx_u16_t{7}) && bytes[0] == 0xef);
    assert(!libk::read_bits(libk::as_bytes(bytes), 0, 65, &value));
    assert(libk::read_bits({}, 0, 0, &value) && value == 0);

    libk::SlotMetadata slots[2];
    libk::SlotPool pool;
    assert(pool.init(slots, 2));
    const auto a = pool.acquire(), b = pool.acquire();
    assert(a && b && !pool.acquire());
    assert(pool.release(a));
    const auto c = pool.acquire();
    assert(c != a && pool.index_of(a) == libk::kNoSlot && !pool.release(a));
    assert(pool.release(b) && pool.release(c));
    slots[0].generation = ~mx_u32_t{0};
    const auto exhausted = pool.acquire();
    assert(pool.release(exhausted));
    assert(pool.index_of(exhausted) == libk::kNoSlot);
}

static void descriptor_tests() {
    mx_u8_t device[] = {18,1,0,2,0,0,0,8,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
    DeviceDescriptor parsed{};
    assert(parse_device(libk::as_bytes(device), MxUsbSpeed::Full, &parsed));
    assert(parsed.vendor == 0x1234 && parsed.product == 0x5678 && parsed.ep0_max_packet == 8);
    assert(!parse_device(libk::as_bytes(device, 17), MxUsbSpeed::Full, &parsed));
    assert(!parse_device(libk::as_bytes(device), MxUsbSpeed::Super, &parsed));
    device[7] = 9;
    assert(parse_device(libk::as_bytes(device), MxUsbSpeed::Super, &parsed) && parsed.ep0_max_packet == 512);

    mx_u8_t config[] = {
        9,2,41,0,2,1,0,0x80,50,
        9,4,0,0,1,3,1,1,0,
        7,5,0x81,3,8,0,10,
        9,4,1,0,1,0xff,0,0,0,
        7,5,0x02,2,64,0,0,
    };
    InterfaceDescriptor interfaces[8]; EndpointDescriptor endpoints[8];
    Configuration tree{interfaces, endpoints, 8, 8, 0, 0, 0, 0, 0, 0};
    assert(parse_configuration(libk::as_bytes(config), MxUsbSpeed::Full, &tree));
    assert(tree.interface_count == 2 && tree.endpoint_count == 2 && endpoints[1].type == MxUsbTransferType::Bulk);
    assert(!parse_configuration(libk::as_bytes(config, 40), MxUsbSpeed::Full, &tree));
    assert(tree.interface_count == 0 && tree.endpoint_count == 0);
    config[18] = 0;
    assert(!parse_configuration(libk::as_bytes(config), MxUsbSpeed::Full, &tree));
    config[18] = 7;
    config[36] = 0x81;
    assert(!parse_configuration(libk::as_bytes(config), MxUsbSpeed::Full, &tree));
    config[36] = 0x02;
    config[34] = 8;
    assert(!parse_configuration(libk::as_bytes(config), MxUsbSpeed::Full, &tree));
    config[34] = 7;
    // Exhaustion cannot publish a partial descriptor tree.
    tree.endpoint_capacity = 1;
    assert(!parse_configuration(libk::as_bytes(config), MxUsbSpeed::Full, &tree));
    tree.endpoint_capacity = 8;
    // Every truncation, and malformed size byte, must fail without reading past the blob.
    for (mx_u32_t n = 0; n < sizeof(config); ++n)
        assert(!parse_configuration(libk::as_bytes(config, n), MxUsbSpeed::Full, &tree));
    const mx_u32_t descriptor_offsets[] = {9, 18, 25, 34};
    for (mx_u32_t at : descriptor_offsets) {
        const auto saved = config[at];
        for (mx_u32_t length = 0; length < 256; ++length) {
            config[at] = static_cast<mx_u8_t>(length);
            if (length != saved) assert(!parse_configuration(libk::as_bytes(config), MxUsbSpeed::Full, &tree));
        }
        config[at] = saved;
    }
}

static void ring_tests() {
    MxUsbSpeed speed{};
    assert(midix::usb::xhci::standard_speed(1, &speed) && speed == MxUsbSpeed::Full);
    assert(midix::usb::xhci::standard_speed(2, &speed) && speed == MxUsbSpeed::Low);
    assert(!midix::usb::xhci::standard_speed(0, &speed));
    alignas(64) midix::usb::xhci::Trb storage[8];
    midix::usb::xhci::ProducerRing ring;
    assert(ring.init(storage, 0x10000, 8));
    for (mx_u32_t i = 0; i < 7; ++i) assert(ring.enqueue({i, 8, midix::usb::xhci::type(1)}));
    assert(!ring.enqueue({}) && ring.available() == 0 && ring.cycle == 0);
    assert(!ring.retire(8) && ring.retire(7));
    assert(ring.enqueue({0x123, 16, midix::usb::xhci::type(1)}));
    assert((storage[0].control & 1) == 0);
    assert(!ring.init(storage, 0x1ffc0, 8));
    mx_u32_t interval = 0;
    assert(midix::usb::xhci::interrupt_interval(MxUsbSpeed::High, 1, &interval) && interval == 0);
    assert(midix::usb::xhci::interrupt_interval(MxUsbSpeed::High, 16, &interval) && interval == 15);
    assert(midix::usb::xhci::interrupt_interval(MxUsbSpeed::Full, 10, &interval) && interval == 7);
    assert(!midix::usb::xhci::interrupt_interval(MxUsbSpeed::High, 17, &interval));
    mx_u32_t written = 0;
    assert(midix::usb::xhci::encode_normal_td(storage, 8, 0x1fff0, 65552, 64, MxUsbDirection::Out, false, &written));
    assert(written == 2 && (storage[0].status & 0x1ffffu) == 16 && (storage[1].status & 0x1ffffu) == 65536);
    assert(midix::usb::xhci::encode_normal_td(storage, 8, 0x10000, 64, 64, MxUsbDirection::Out, true, &written));
    assert(written == 2 && (storage[1].status & 0x1ffffu) == 0);
    assert(!midix::usb::xhci::encode_normal_td(storage, 1, 0x1fff0, 65552, 64, MxUsbDirection::In, false, &written));
    assert(midix::usb::xhci::normal_trb_count(~mx_u64_t{0}, 2, 64, false) == 0);
    assert(midix::usb::xhci::normal_trb_count(~mx_u64_t{0}, 1, 1, true) == 0);
    midix::usb::xhci::EventBacklog backlog;
    assert(backlog.init(storage, 8));
    for (mx_u32_t i = 0; i < 8; ++i) assert(backlog.push({i, 0, 0}));
    assert(!backlog.push({99, 0, 0}));
    midix::usb::xhci::Trb event{};
    for (mx_u32_t i = 0; i < 8; ++i) assert(backlog.pop(&event) && event.parameter == i);
    assert(!backlog.pop(&event) && backlog.depth() == 0);
}

static void lifecycle_tests() {
    RoleLifecycle role;
    const auto features = MX_USB_FEATURE_HOST | MX_USB_FEATURE_DEVICE;
    assert(role.request(MxUsbRole::Host, features) == MX_OK && role.state() == RoleState::Preparing);
    assert(role.role() == MxUsbRole::Detached);
    assert(role.request(MxUsbRole::Device, features) == MX_ERR_BUSY);
    assert(role.prepared() == MX_OK && role.role() == MxUsbRole::Host);
    const auto generation = role.generation();
    assert(role.request(MxUsbRole::Device, features) == MX_OK && role.state() == RoleState::Quiescing);
    assert(role.prepared() == MX_ERR_INVALID && role.role() == MxUsbRole::Host);
    assert(role.quiesced() == MX_OK && role.role() == MxUsbRole::Detached);
    assert(role.prepared() == MX_OK && role.generation() > generation);
    role.fault();
    assert(role.recover(false) == MX_ERR_BUSY && role.recover(true) == MX_OK);
    assert(role.request(MxUsbRole::Device, MX_USB_FEATURE_HOST) == MX_ERR_NOT_IMPLEMENTED);
}

static void hid_tests() {
    BootKeyboardDecoder decoder;
    UsbBootKeyboardReport report{2, 0, {4, 4, 0, 0, 0, 0}};
    assert(decoder.prepare(libk::as_bytes(&report, sizeof(report))));
    assert(decoder.event()->keycode == 0xe1 && decoder.event()->type == INPUT_KEY_DOWN);
    assert(!decoder.prepare(libk::as_bytes(&report, sizeof(report))));
    // A full output ring does not advance state or duplicate already sent events.
    assert(decoder.previous().modifiers == 0);
    assert(decoder.advance() && decoder.event()->keycode == 4);
    assert(decoder.advance() && !decoder.pending());
    assert(decoder.previous().modifiers == 2);
    report = {0, 0, {1, 1, 1, 1, 1, 1}};
    assert(decoder.prepare(libk::as_bytes(&report, sizeof(report))));
    assert(decoder.event()->keycode == 0xe1 && decoder.event()->type == INPUT_KEY_UP);
    assert(decoder.advance() && !decoder.pending());
    assert(decoder.previous().keys[0] == 4);
    report = {};
    assert(decoder.prepare(libk::as_bytes(&report, sizeof(report))));
    assert(decoder.event()->keycode == 4 && decoder.event()->type == INPUT_KEY_UP);
    assert(decoder.advance() && !decoder.pending());
    assert(!decoder.prepare(libk::as_bytes(&report, sizeof(report) - 1)));
}


static void exec_tests() {
    alignas(64) mx_u8_t memory[16384]{};
    mx_u8_t payload[64]{};
    mochi::Arena arena(memory, sizeof(memory));
    TransferEngine engine;
    assert(engine.init(arena, {4, 4, 8, 64, (1u << 0) | (1u << 2) | (1u << 3)}));
    ExecEntry records[8];
    ExecTable exec;
    assert(exec.init(records, 8));

    alignas(64) midix::usb::xhci::Trb storage[16];
    midix::usb::xhci::ProducerRing ring;
    assert(ring.init(storage, 0x4000, 16));

    auto event_trb = [](mx_u32_t slot, mx_u32_t endpoint, mx_u64_t parameter, mx_u32_t code,
                        mx_u32_t residual) {
        midix::usb::xhci::Trb e{};
        e.parameter = parameter;
        e.status = (code << 24) | residual;
        e.control = (32u << 10) | ((slot & 0xffu) << 24) | ((endpoint & 0x1fu) << 16);
        return e;
    };

    const SessionScope owner{3, 1, 1};
    midix::usb::EndpointDescriptor descriptor{};
    descriptor.address = 0x81; descriptor.type = MxUsbTransferType::Interrupt;
    descriptor.max_packet = 8; descriptor.interval = 10; descriptor.transactions = 1;
    MxUsbToken endpoint = 0, buffer = 0;
    assert(engine.install_endpoint({owner, 1, descriptor, MxUsbSpeed::Full, 0, ControlPolicy::None}, &endpoint) == MX_OK);
    assert(engine.install_buffer({owner, libk::as_bytes(payload), 0x40000, BufferRead | BufferWrite,
                                  CacheMode::NonCacheable, true}, &buffer) == MX_OK);

    // Success: the terminal event publishes through the engine and reclaim
    // retires the ring slot and the ledger token.
    MxUsbSubmit request{1, endpoint, buffer, 0, 8, 0, 0, 0};
    MxUsbToken token = 0;
    PreparedTransfer prepared{};
    assert(engine.submit(owner, request, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(1)});
    assert(phys && exec.record(token, 1, 3, 3, &ring, 1, phys, phys, 0, 8) == MX_OK);
    assert(!exec.on_event({0, 0, midix::usb::xhci::type(1)}, engine)); // not a transfer event
    assert(!exec.on_event(event_trb(2, 3, phys, 1, 0), engine));       // wrong slot: unmatched
    assert(engine.completion(token) == nullptr);
    assert(exec.on_event(event_trb(3, 3, phys, 1, 0), engine));
    assert(engine.completion(token) && engine.completion(token)->status == MxUsbStatus::Success);
    assert(engine.completion(token)->actual_length == 8);
    auto* entry = exec.find(token);
    assert(entry && (entry->flags & kExecDone));
    assert(exec.reclaim(*entry, engine) == MX_OK);
    assert(exec.find(token) == nullptr && ring.outstanding == 0);

    // A short single-TRB transfer (residual > 0) publishes Short with the
    // accumulated length.
    request.request_id = 2;
    assert(engine.submit(owner, request, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto short_phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(1)});
    assert(short_phys && exec.record(token, 2, 3, 3, &ring, 1, short_phys, short_phys, 0, 8) == MX_OK);
    assert(exec.on_event(event_trb(3, 3, short_phys, 13, 3), engine));
    assert(engine.completion(token) && engine.completion(token)->status == MxUsbStatus::Short);
    assert(engine.completion(token)->actual_length == 5);
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);

    // Stall is a terminal non-success result the caller can observe.
    request.request_id = 3;
    assert(engine.submit(owner, request, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto stall_phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(1)});
    assert(stall_phys && exec.record(token, 3, 3, 3, &ring, 1, stall_phys, stall_phys, 0, 8) == MX_OK);
    assert(exec.on_event(event_trb(3, 3, stall_phys, 6, 0), engine));
    assert(engine.completion(token) && engine.completion(token)->status == MxUsbStatus::Stall);
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);

    // A control transfer keeps its short data stage live until the status
    // stage completes, then publishes the accumulated length.
    descriptor = {};
    descriptor.type = MxUsbTransferType::Control; descriptor.max_packet = 8;
    MxUsbToken control_ep = 0;
    assert(engine.install_endpoint({owner, 1, descriptor, MxUsbSpeed::Full, 2,
                                    ControlPolicy::Bus, 99}, &control_ep) == MX_OK);
    MxUsbControlSubmit control{100, 99, buffer, 0, {0x80, 6, 1, 0, 8}, 0, 0};
    assert(engine.submit_control(owner, control_ep, control, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto setup_phys = ring.enqueue({0x80ull, 8, midix::usb::xhci::type(2)});
    const auto data_phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(3)});
    const auto status_phys = ring.enqueue({0, 0, midix::usb::xhci::type(4)});
    assert(setup_phys && data_phys && status_phys &&
           exec.record(token, 100, 3, 1, &ring, 3, setup_phys, status_phys, data_phys, 8) == MX_OK);
    assert(exec.on_event(event_trb(3, 1, data_phys, 13, 2), engine));  // short data stage
    assert(engine.completion(token) == nullptr);                       // still owned by hardware
    assert(exec.on_event(event_trb(3, 1, status_phys, 1, 0), engine)); // status stage
    assert(engine.completion(token) && engine.completion(token)->status == MxUsbStatus::Short);
    assert(engine.completion(token)->actual_length == 6);
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);

    // A stall at the setup stage terminates the transfer against the head TRB.
    assert(engine.submit_control(owner, control_ep, control, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto stall_setup = ring.enqueue({0x80ull, 8, midix::usb::xhci::type(2)});
    const auto stall_data = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(3)});
    const auto stall_status = ring.enqueue({0, 0, midix::usb::xhci::type(4)});
    assert(stall_setup && stall_data && stall_status &&
           exec.record(token, 101, 3, 1, &ring, 3, stall_setup, stall_status, stall_data, 8) == MX_OK);
    assert(exec.on_event(event_trb(3, 1, stall_setup, 6, 0), engine));
    assert(engine.completion(token) && engine.completion(token)->status == MxUsbStatus::Stall);
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);

    // Live records refuse reclaim; cancellation before completion keeps the
    // cancellation as the terminal result.
    request.request_id = 4;
    assert(engine.submit(owner, request, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto live_phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(1)});
    assert(live_phys && exec.record(token, 4, 3, 3, &ring, 1, live_phys, live_phys, 0, 8) == MX_OK);
    assert(exec.reclaim(*exec.find(token), engine) == MX_ERR_BUSY);
    assert(engine.cancel(owner, 4) == MX_OK && engine.state(token) == TransferState::Stopping);
    assert(exec.on_event(event_trb(3, 3, live_phys, 1, 0), engine));
    assert(engine.completion(token) && engine.completion(token)->status == MxUsbStatus::Cancelled);
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);

    // A consumed record cannot be reclaimed again by a replayed event.
    request.request_id = 5;
    assert(engine.submit(owner, request, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto replay_phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(1)});
    assert(replay_phys && exec.record(token, 5, 3, 3, &ring, 1, replay_phys, replay_phys, 0, 8) == MX_OK);
    assert(exec.on_event(event_trb(3, 3, replay_phys, 1, 0), engine));
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);
    assert(!exec.on_event(event_trb(3, 3, replay_phys, 1, 0), engine));

    // An abandoned wait (timeout) keeps its record pinned; a late completion
    // still publishes the retained timeout result for the reclaim sweep.
    request.request_id = 6;
    assert(engine.submit(owner, request, 1, &token) == MX_OK);
    assert(engine.start(token, &prepared) == MX_OK);
    const auto late_phys = ring.enqueue({prepared.device_address, 8, midix::usb::xhci::type(1)});
    assert(late_phys && exec.record(token, 6, 3, 3, &ring, 1, late_phys, late_phys, 0, 8) == MX_OK);
    exec.find(token)->flags |= kExecOrphan;
    assert(engine.stop(token, MxUsbStatus::Timeout) == MX_OK);
    assert(exec.on_event(event_trb(3, 3, late_phys, 1, 0), engine));
    const auto* timeout_completion = engine.completion(token);
    assert(timeout_completion && timeout_completion->status == MxUsbStatus::Timeout);
    assert(exec.reclaim(*exec.find(token), engine) == MX_OK);

    // Duplicate tokens are refused; a full table reports overflow.
    assert(exec.record(0x100, 7, 3, 3, &ring, 1, 0x1000, 0x1000, 0, 8) == MX_OK);
    assert(exec.record(0x100, 8, 3, 3, &ring, 1, 0x1000, 0x1000, 0, 8) == MX_ERR_BUSY);
    for (mx_u32_t i = 1; i < 8; ++i)
        assert(exec.record(0x100 + i, 8 + i, 3, 3, &ring, 1, 0x1000 + i, 0x1000 + i, 0, 8) == MX_OK);
    assert(exec.record(0x200, 30, 3, 3, &ring, 1, 0x2000, 0x2000, 0, 8) == MX_ERR_OVERFLOW);
}

static void engine_tests() {
    alignas(64) mx_u8_t memory[32768]{};
    mx_u8_t payload[256]{};
    mochi::Arena arena(memory, sizeof(memory));
    TransferEngine engine;
    assert(engine.init(arena, {8, 8, 4, 4096, (1u << 0) | (1u << 2) | (1u << 3)}));
    const auto allocated = arena.used();
    const SessionScope owner{7, 1, 1}, stranger{8, 1, 1}, restarted{7, 2, 1};
    EndpointDescriptor descriptor{};
    descriptor.address = 0x81; descriptor.type = MxUsbTransferType::Interrupt;
    descriptor.max_packet = 8; descriptor.interval = 10; descriptor.transactions = 1;
    MxUsbToken endpoint = 0, buffer = 0, transfer = 0;
    assert(engine.install_endpoint({owner, 1, descriptor, MxUsbSpeed::Full, 0, ControlPolicy::None}, &endpoint) == MX_OK);
    assert(engine.install_buffer({owner, libk::as_bytes(payload), 0x800000, BufferRead | BufferWrite,
                                  CacheMode::NonCacheable, true}, &buffer) == MX_OK);
    MxUsbToken rejected = 0;
    assert(engine.install_buffer({owner, libk::as_bytes(payload), 0x900000, BufferWrite,
                                  CacheMode::NonCoherent, true}, &rejected) == MX_ERR_NOT_IMPLEMENTED);
    MxUsbSubmit request{1, endpoint, buffer, 0, 8, 100, 0, 0};
    assert(engine.submit(stranger, request, 1, &transfer) == MX_ERR_PERM);
    assert(engine.submit(restarted, request, 1, &transfer) == MX_ERR_PERM);
    assert(engine.submit(owner, request, 100, &transfer) == MX_ERR_INVALID);
    assert(engine.submit(owner, request, 1, &transfer) == MX_OK);
    request.request_id = 2;
    assert(engine.submit(owner, request, 1, &rejected) == MX_ERR_BUSY);
    PreparedTransfer prepared{};
    assert(engine.start(transfer, &prepared) == MX_OK && prepared.request.request_id == 1);
    assert(prepared.direct_dma && prepared.device_address == 0x800000 && prepared.payload.size == 8);
    assert(engine.cancel(owner, 1) == MX_OK && engine.state(transfer) == TransferState::Stopping);
    assert(!engine.completion(transfer) && engine.retire(transfer) == MX_ERR_BUSY);
    assert(engine.finish(transfer, MxUsbStatus::Success, 8) == MX_OK);
    assert(engine.completion(transfer)->status == MxUsbStatus::Cancelled);
    assert(engine.finish(transfer, MxUsbStatus::Success, 8) == MX_ERR_BUSY);
    // Completion-ring backpressure retains ownership until publication.
    assert(engine.submit(owner, request, 1, &rejected) == MX_ERR_BUSY);
    assert(engine.retire(transfer) == MX_OK);
    assert(engine.state(transfer) == TransferState::Free);
    assert(engine.submit(owner, request, 1, &transfer) == MX_OK);
    engine.expire(100);
    assert(engine.completion(transfer)->status == MxUsbStatus::Timeout && engine.retire(transfer) == MX_OK);
    request.request_id = 3; request.deadline_ns = 0;
    assert(engine.submit(owner, request, 1, &transfer) == MX_OK);
    assert(engine.start(transfer, &prepared) == MX_OK);
    engine.close_session(owner);
    assert(engine.state(transfer) == TransferState::Stopping && !engine.completion(transfer));
    assert(engine.submit(owner, request, 1, &rejected) == MX_ERR_NOT_FOUND);
    assert(engine.stopped(transfer) == MX_OK && engine.retire(transfer) == MX_OK);
    assert(engine.submit(owner, request, 1, &rejected) == MX_ERR_NOT_FOUND);

    // Repeated setup-free transfers neither allocate nor resurrect stale tokens.
    assert(engine.install_endpoint({owner, 1, descriptor, MxUsbSpeed::Full, 0, ControlPolicy::None}, &endpoint) == MX_OK);
    assert(engine.install_buffer({owner, libk::as_bytes(payload), 0x800000, BufferRead | BufferWrite,
                                  CacheMode::Coherent, true}, &buffer) == MX_OK);
    request.endpoint = endpoint; request.buffer = buffer;
    for (mx_u32_t i = 0; i < 1000; ++i) {
        request.request_id = i + 10;
        assert(engine.submit(owner, request, 1, &transfer) == MX_OK);
        assert(engine.start(transfer, &prepared) == MX_OK);
        assert(engine.finish(transfer, MxUsbStatus::Success, 4) == MX_OK);
        assert(engine.completion(transfer)->status == MxUsbStatus::Short);
        assert(engine.retire(transfer) == MX_OK && arena.used() == allocated);
    }
    request.offset = 255; request.length = 2;
    assert(engine.submit(owner, request, 1, &transfer) == MX_ERR_INVALID);
    request.offset = 0; request.length = 0; request.buffer = 0;
    for (mx_u32_t i = 0; i < 4; ++i) {
        request.request_id = i + 2000;
        assert(engine.submit(owner, request, 1, &transfer) == MX_OK);
    }
    request.request_id = 3000;
    assert(engine.submit(owner, request, 1, &transfer) == MX_ERR_WOULD_BLOCK);
    assert(engine.available() == 0);
    // Out-of-order retirement across hash collisions must retain other IDs.
    for (mx_u32_t i = 0; i < 4; ++i) {
        const auto token = engine.token_at(i);
        assert(engine.stop(token, MxUsbStatus::Cancelled) == MX_OK);
        assert(engine.retire(token) == MX_OK);
    }
    MxUsbToken colliding[4]{};
    for (mx_u32_t i = 0; i < 4; ++i) {
        request.request_id = 4000 + i * 8;
        assert(engine.submit(owner, request, 1, colliding + i) == MX_OK);
    }
    assert(engine.cancel(stranger, 4000) == MX_ERR_NOT_FOUND);
    assert(engine.cancel(owner, 4008) == MX_OK && engine.retire(colliding[1]) == MX_OK);
    assert(engine.cancel(owner, 4024) == MX_OK && engine.retire(colliding[3]) == MX_OK);
    assert(engine.cancel(owner, 4000) == MX_OK && engine.retire(colliding[0]) == MX_OK);
    assert(engine.cancel(owner, 4016) == MX_OK && engine.retire(colliding[2]) == MX_OK);

    // Interface EP0 grants cannot send device-wide standard requests or address
    // another interface, even though they share one physical control pipe.
    descriptor = {}; descriptor.type = MxUsbTransferType::Control; descriptor.max_packet = 8;
    MxUsbToken control_endpoint = 0;
    assert(engine.install_endpoint({owner, 1, descriptor, MxUsbSpeed::Full, 2,
                                    ControlPolicy::InterfaceClass, 99}, &control_endpoint) == MX_OK);
    MxUsbControlSubmit control{5000, 99, 0, 0, {0x21, 11, 0, 2, 0}, 0, 0};
    assert(engine.submit_control(owner, control_endpoint, control, 1, &transfer) == MX_OK);
    assert(engine.start(transfer, &prepared) == MX_OK && prepared.setup.request == 11);
    assert(engine.finish(transfer, MxUsbStatus::Success, 0) == MX_OK && engine.retire(transfer) == MX_OK);
    control.interface = 100;
    assert(engine.submit_control(owner, control_endpoint, control, 1, &transfer) == MX_ERR_PERM);
    control.interface = 99; control.setup.index = 3;
    assert(engine.submit_control(owner, control_endpoint, control, 1, &transfer) == MX_ERR_PERM);
    control.setup.index = 2; control.setup.request_type = 0;
    assert(engine.submit_control(owner, control_endpoint, control, 1, &transfer) == MX_ERR_PERM);
    descriptor.type = static_cast<MxUsbTransferType>(255); descriptor.address = 0x81;
    assert(engine.install_endpoint({owner, 1, descriptor, MxUsbSpeed::Full, 0, ControlPolicy::None}, &rejected) == MX_ERR_INVALID);
}

int main() {
    helper_tests(); descriptor_tests(); ring_tests(); lifecycle_tests(); hid_tests(); engine_tests();
    exec_tests();
    std::puts("usb core probe ok");
}

#include "mochi/va_layout.h"
namespace {
using namespace mochi::va;
constexpr auto ring_span = mx_ring_page_bytes(MX_RING_ORDER_MAX);
static_assert(init::kVaSvcCtlReqRing + ring_span <= init::kVaSvcCtlReplyRing);
static_assert(msh::kVaFsReplyRing + ring_span <= msh::kVaSvcCmdRing);
static_assert(msh::kVaSvcCmdRing + ring_span <= msh::kVaSvcReplyRing);
static_assert(msh::kVaSvcReplyRing + ring_span <= msh::kVaSvcRegistry);
static_assert(fs::kVaBlkReqRing + ring_span <= fs::kVaBlkReplyRing);
static_assert(blk::kVaReqRing + ring_span <= blk::kVaReplyRing);
static_assert(hid::kVaHostTx + ring_span <= hid::kVaHostRx);
static_assert(hid::kVaHostRx + ring_span <= hid::kVaInputRing);
static_assert(xhci::kVaControllerTx + ring_span <= xhci::kVaControllerRx);
static_assert(usb_host::kVaControllerRx + ring_span <= usb_host::kVaClassTx);
static_assert(usb_host::kVaClassTx + ring_span <= usb_host::kVaClassRx);
static_assert(input::kVaHidRing + ring_span <= input::kVaTtyRing);
static_assert(demo_ping::kVaSendRing + ring_span <= demo_ping::kVaRecvRing);
static_assert(demo_ping::kVaRecvRing + ring_span <= demo_ping::kVaConsoleRing);
static_assert(demo_ping::kVaConsoleRing + ring_span <= demo_ping::kVaNsRequestRing);
static_assert(demo_pong::kVaRecvRing + ring_span <= demo_pong::kVaSendRing);
static_assert(demo_pong::kVaSendRing + ring_span <= demo_pong::kVaLogRing);
}
// Exercise production IPC helpers with deterministic syscall callbacks.
#include <cassert>
#include <cstddef>
#include "mochi/channel.h"
#include "mochi/fs.h"
#include "mochi/input.h"
#include "mochi/nameservice.h"
#include "mochi/usb.h"
#include "mochi/framebuffer.h"
#include "mochi/driver.h"
#include "mochi/inbox.h"

namespace {
constexpr uint32_t kOrder = 2;
constexpr uint32_t kType = 17;
struct Storage {
    RingPage page{};
    RingSlot slots[1u << kOrder]{};
};
static_assert(__builtin_offsetof(Storage, slots) == MX_RING_HEADER_SIZE);
Storage storage;
mochi::Ring ring{&storage.page, kOrder, 1};

void reset() {
    storage = {};
    host_yields = host_notifies = 0;
    host_now = 0;
    host_on_yield = host_on_read = nullptr;
    host_on_wait = nullptr;
}

Storage inbox_storage[8];
RingPage* inbox_rings[8];

void inbox_wait_races() {
    reset();
    for (uint32_t i = 0; i < 8; ++i) {
        inbox_storage[i] = {};
        inbox_rings[i] = &inbox_storage[i].page;
    }
    const mochi::InboxWait inbox{inbox_rings, 8, kOrder, 1};
    const mochi::Ring last{inbox_rings[7], kOrder, 1};

    // A queued request skips the syscall; waiting must not consume its payload.
    assert(last.try_push_msg(kType, 73u));
    host_on_wait = []() -> mx_i64_t { assert(false); return MX_ERR_DEAD; };
    assert(inbox.wait(0) == MX_OK && last.depth() == 1);
    RingSlot slot{};
    assert(last.try_pop(&slot));

    // A publication after the final head check claims a sleep flag and changes
    // the expected counter. Every ring, including the last one, must be armed.
    host_on_wait = []() -> mx_i64_t {
        for (const auto* page : inbox_rings) assert(page->flags.receiver_about_to_sleep == 1);
        assert((mochi::Ring{inbox_rings[7], kOrder, 1}.try_push_msg(kType, 91u)));
        assert(host_notifies != host_wait_expected);
        return MX_OK;
    };
    assert(inbox.wait(0) == MX_OK && last.depth() == 1);
    for (const auto* page : inbox_rings) assert(!page->flags.receiver_about_to_sleep);
    assert(last.try_pop(&slot));

    // Exit/session publication is independent of ring flags. Its notification
    // between the caller's event scan and sleep must satisfy the old counter.
    const auto expected = sys_doorbell_read(1);
    mx_doorbell_notify(1);
    host_on_wait = []() -> mx_i64_t {
        assert(host_notifies != host_wait_expected);
        assert(host_wait_deadline == 1234);
        return MX_OK;
    };
    assert(inbox.wait(expected, 1234) == MX_OK);

    // A failed wait disarms every producer too; a replacement generation may
    // remap these addresses immediately after the caller regains control.
    host_on_wait = []() -> mx_i64_t { return MX_ERR_DEAD; };
    assert(inbox.wait(host_notifies) == MX_ERR_DEAD);
    for (const auto* page : inbox_rings) assert(!page->flags.receiver_about_to_sleep);

    // Inactive sessions and zero-client servers still wait for lifecycle events.
    inbox_rings[3] = nullptr;
    host_on_wait = nullptr;
    assert(inbox.wait(host_notifies, 5678) == MX_OK && host_now == 5678);
    assert((mochi::InboxWait{nullptr, 0, kOrder, 1}.wait(host_notifies, 6789)) == MX_OK);
    assert(host_now == 6789 && host_yields == 0);
    reset();
}
void enqueue() {
    const uint32_t value = 42;
    assert(ring.try_push_msg(kType, value));
}
void drain_one() {
    RingSlot slot{};
    assert(ring.try_pop(&slot));
}
mx_i64_t wake_and_enqueue() {
    assert(storage.page.flags.receiver_about_to_sleep == 1);
    enqueue();
    return MX_OK;
}

void invalid_and_full() {
    reset();
    const mochi::Ring missing{};
    assert(!missing.try_push_msg(kType, 1u));
    assert(!missing.push_msg(kType, 1u));
    RingSlot slot{};
    assert(!ring_pop(&storage.page, 64, &slot));
    assert(!ring_try_push(&storage.page, 32, kType, &slot, 1, 1));
    assert(!ring.try_push(kType, nullptr, 1));
    assert(!ring.wait_pop(nullptr));
    assert(!ring.pop_bounded(nullptr, 100));
    assert(!ring.push_bounded(kType, nullptr, 1, 100));
    assert(ring.push_bytes(kType, nullptr, 1) == 0);
    assert(host_yields == 0 && ring.depth() == 0);

    for (unsigned i = 0; i < ring.capacity(); ++i) enqueue();
    assert(!ring.push_msg(kType, 7u, 0) && host_yields == 0);
    assert(!ring.push_msg(kType, 7u, 2) && host_yields == 2);
    host_on_yield = drain_one;
    assert(ring.push_msg(kType, 7u, ~0u));
    assert(host_yields == 3 && ring.depth() == ring.capacity());
}

void byte_runs_and_wrap() {
    reset();
    uint8_t data[MX_RING_INLINE_PAYLOAD * 5 + 7];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(i);
    const unsigned prefix = MX_RING_INLINE_PAYLOAD * ring.capacity();
    assert(ring.push_bytes(kType, data, sizeof(data)) == prefix);
    assert(host_yields == 0);
    RingSlot slot{};
    for (unsigned i = 0; i < ring.capacity(); ++i) {
        assert(ring.try_pop(&slot));
        assert(slot.seq == i && slot.length == MX_RING_INLINE_PAYLOAD);
        for (unsigned j = 0; j < slot.length; ++j) assert(slot.payload[j] == data[i * slot.length + j]);
    }
    assert(ring.push_bytes(kType, data + prefix, sizeof(data) - prefix) == sizeof(data) - prefix);
    assert(ring.try_pop(&slot) && slot.length == MX_RING_INLINE_PAYLOAD);
    assert(ring.try_pop(&slot) && slot.length == 7);
    for (unsigned j = 0; j < 7; ++j) assert(slot.payload[j] == data[prefix + MX_RING_INLINE_PAYLOAD + j]);
    for (unsigned j = 7; j < MX_RING_INLINE_PAYLOAD; ++j) assert(slot.payload[j] == 0);
    storage.page.head.v = storage.page.tail.v = ~mx_u64_t{0};
    enqueue();
    uint32_t out = 0;
    assert(ring.try_pop_msg(kType, &out) && out == 42 && ring.depth() == 0);
}

void sleep_and_typed_reads() {
    reset();
    ring.publish_sleep(0);
    enqueue();
    enqueue();
    assert(host_notifies == 1);
    drain_one();
    drain_one();
    host_on_read = enqueue; // publication between empty check and sleep flag
    uint32_t out = 0;
    assert(ring.wait_pop_msg(kType, &out) && out == 42);
    assert(storage.page.flags.receiver_about_to_sleep == 0);
    host_on_read = nullptr;
    host_on_wait = wake_and_enqueue;
    assert(ring.wait_pop_msg(kType, &out) && out == 42 && host_notifies == 2);
    host_on_wait = nullptr;
    assert(!ring.wait_pop_msg(kType, &out));
    assert(storage.page.flags.receiver_about_to_sleep == 0);
    assert(ring.try_push_msg(kType + 1, 7u));
    assert(ring.try_push(kType, "x", 1));
    enqueue();
    assert(ring.wait_pop_msg(kType, &out) && out == 42 && ring.depth() == 0);
    assert(ring.try_push_msg(kType + 1, 7u));
    out = 99;
    assert(!ring.try_pop_msg(kType, &out) && out == 99 && ring.depth() == 0);
}

void protocol_replies() {
    reset();
    Storage tx;
    const mochi::SpscEndpoint endpoint{&tx.page, &storage.page, kOrder, 1, 2};
    const mochi::fs::Client fs{endpoint};
    const mx_i32_t error = MX_ERR_NOT_FOUND;
    assert(ring.try_push_msg(FS_ERROR, error));
    FsOpenReply reply{};
    reply.handle = 123;
    assert(fs.wait_open_reply(&reply));
    assert(reply.status == error && reply.handle == 0);
    assert(ring.try_push_msg(FS_ERROR, error));
    mx_i32_t status = 0;
    assert(fs.wait_status_reply(FS_SYNC_REPLY, &status) && status == error);
    const mx_status_t ns_error = error;
    assert(ring.try_push_msg(MSG_NS_ERROR, ns_error));
    assert(mochi::nameservice::wait_resolve_reply(endpoint, nullptr) == error);
    MxEndpointDesc desc{};
    desc.magic = MX_ENDPOINT_MAGIC;
    assert(ring.try_push_msg(MSG_NS_RESOLVE_REPLY, desc));
    MxEndpointDesc decoded{};
    assert(mochi::nameservice::wait_resolve_reply(endpoint, &decoded) == MX_OK);
    assert(decoded.magic == desc.magic);

    const mochi::Channel channel{endpoint};
    for (unsigned i = 0; i < ring.capacity(); ++i) assert(endpoint.tx().try_push_msg(kType, 1u));
    uint32_t out = 0;
    assert(!channel.call(kType, 1u, kType, &out, 2));
    assert(host_yields == 2); // deadline bounds the send as well as the receive
    enqueue();
    assert(channel.depth() == ring.depth());
    assert(!channel.receive(kType, static_cast<uint32_t*>(nullptr)));
    assert(channel.depth() == 1); // invalid output must not consume a reply
    drain_one();
    assert(ring.try_push_msg(kType + 1, 7u));
    assert(ring.try_push_msg(kType + 1, 7u));
    assert(!channel.receive(kType, &out, 2));
    assert(channel.depth() == 1); // unrelated traffic cannot bypass the deadline
}
void usb_transfer_hot_path() {
    reset();
    Storage reply;
    mochi::usb::TransferClient client;
    static_cast<mochi::SpscEndpoint&>(client) = {&storage.page, &reply.page, kOrder, 1, 2};
    mochi::usb::TransferServer server;
    static_cast<mochi::SpscEndpoint&>(server) = {&reply.page, &storage.page, kOrder, 2, 1};
    const MxUsbSubmit request{99, 1, 2, 8, 16, 1000, 0, 0};
    for (unsigned i = 0; i < ring.capacity(); ++i) assert(client.try_submit(request));
    assert(!client.try_submit(request));
    assert(host_yields == 0 && host_notifies == 0);
    RingSlot slot{};
    assert(server.try_recv(&slot));
    MxUsbSubmit decoded{};
    assert(mochi::message_get(slot, &decoded, MX_USB_TRANSFER_SUBMIT));
    assert(decoded.request_id == 99 && decoded.offset == 8 && decoded.length == 16);
    const MxUsbCompletion completion{99, 1, MxUsbStatus::Success, 16, 0, 0};
    assert(server.try_complete(completion));
    MxUsbCompletion result{};
    assert(client.try_completion(&result) && result.request_id == 99 && result.actual_length == 16);
    assert(host_yields == 0 && host_notifies == 0);
}

void cold_driver_bindings() {
    // Exercise the same named startup mappings the standalone I2C service uses.
    // Kernel mappings are a syscall seam; capability metadata and endpoint
    // assembly remain production helpers, including failures and asymmetric order.
    MxBootInfo boot{};
    boot.magic = MX_BOOT_MAGIC;
    boot.version = MX_BOOT_VERSION;
    auto add = [&](const char* name, mx_u32_t type, mx_handle_t cap, mx_u64_t value = 0) {
        auto& entry = boot.entries[boot.entry_count++];
        entry.type = type; entry.handle = cap; entry.aux0 = value;
        libk::str::copy_into(entry.name, sizeof(entry.name), name);
    };
    add("i2c.0.tx", MX_BOOT_ENTRY_RING, 11, 6);
    add("i2c.0.rx", MX_BOOT_ENTRY_RING, 12, 6);
    add("i2c.0.tx.db", MX_BOOT_ENTRY_DOORBELL, 13);
    add("i2c.0.rx.db", MX_BOOT_ENTRY_DOORBELL, 14);
    add("i2c.0.address", MX_BOOT_ENTRY_VALUE, 0, 33);
    add("i2c.mmio", MX_BOOT_ENTRY_HANDLE, 15);
    host_on_map_page = [](CapHandle cap, mx_vaddr_t, mx_rights_t rights) -> mx_status_t {
        assert(rights == (MX_RIGHT_READ | MX_RIGHT_WRITE));
        return cap == 11 || cap == 12 ? MX_OK : MX_ERR_NOT_FOUND;
    };
    const auto link = mochi::driver::link(&boot, "i2c.0", 0x1b300000);
    assert(link.valid() && link.order == 6 && link.tx_doorbell == 13 && link.rx_doorbell == 14);
    assert(mochi::driver::value(&boot, "i2c.0.address") == 33);
    assert(!mochi::driver::link(&boot, "missing", 0x1b300000).valid());
    boot.entries[1].aux0 = 7;
    assert(!mochi::driver::link(&boot, "i2c.0", 0x1b300000).valid());
    host_on_cap_info = [](CapHandle cap, MxCapInfo* info) -> mx_status_t {
        assert(cap == 15);
        *info = {}; info->type = MX_CAP_MMIO; info->attr = MX_MMIO_DEVICE_nGnRnE; info->size = 4096;
        return MX_OK;
    };
    host_on_map_mmio = [](CapHandle cap, mx_vaddr_t va, mx_rights_t rights) -> mx_status_t {
        assert(cap == 15 && va == 0x1b200000 && rights == (MX_RIGHT_READ | MX_RIGHT_WRITE));
        return MX_OK;
    };
    assert(mochi::driver::registers(&boot, "i2c.mmio", 0x1b200000).size == 4096);
    host_on_map_mmio = nullptr;
    assert(!mochi::driver::registers(&boot, "i2c.mmio", 0x1b200000).base);
    host_on_map_page = nullptr; host_on_cap_info = nullptr;
}
} // namespace

static void batches() {
    reset();
    RingSlot frames[6]{}, out[6]{};
    for (uint32_t i=0;i<6;++i) {
        assert(mochi::message_put(&frames[i],kType,i));
        frames[i].seq=700+i;
        for (uint32_t j=4;j<MX_RING_INLINE_PAYLOAD;++j) frames[i].payload[j]=0xaa;
    }
    storage.page.head.v=storage.page.tail.v=~uint64_t{0}-1;
    ring.publish_sleep(0);
    assert(ring.try_push_frames(frames,6)==4 && host_notifies==1);
    assert(ring.try_pop_batch(out,3)==3);
    for (uint32_t i=0;i<3;++i) {
        uint32_t value=99;
        assert(mochi::message_get(out[i],&value,kType) && value==i && out[i].seq==700+i);
        for (uint32_t j=4;j<MX_RING_INLINE_PAYLOAD;++j) assert(out[i].payload[j]==0);
    }
    frames[1].length=49;
    assert(ring.try_push_frames(frames,3)==1);
    assert(ring.try_pop_batch(out,6)==2 && out[0].seq==703 && out[1].seq==700);
    assert(!ring.try_pop_batch(out,0) && !ring.try_push_frames(nullptr,1));
    assert(!ring.try_pop_batch(nullptr,1));
}

int main() {
    reset();
    RingSlot frame{}, received{};
    mochi::message_put(&frame, kType, 42u);
    frame.seq = 789;
    assert(ring_try_push_frame(&storage.page, kOrder, frame, 1));
    assert(ring.try_pop(&received) && received.seq == frame.seq);
    invalid_and_full();
    byte_runs_and_wrap();
    batches();
    inbox_wait_races();
    sleep_and_typed_reads();
    protocol_replies();
    usb_transfer_hot_path();
    cold_driver_bindings();
}

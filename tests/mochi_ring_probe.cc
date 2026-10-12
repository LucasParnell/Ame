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
static_assert(hid::kVaXhciRing + ring_span <= hid::kVaInputRing);
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
} // namespace

int main() {
    reset();
    RingSlot frame{}, received{};
    mochi::message_put(&frame, kType, 42u);
    frame.seq = 789;
    assert(ring_try_push_frame(&storage.page, kOrder, frame, 1));
    assert(ring.try_pop(&received) && received.seq == frame.seq);
    invalid_and_full();
    byte_runs_and_wrap();
    sleep_and_typed_reads();
    protocol_replies();
    usb_transfer_hot_path();
}

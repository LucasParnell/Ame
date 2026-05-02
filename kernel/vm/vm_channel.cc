#include "anko/vm.h"
#include "libk/string.h"

namespace vm {

namespace {

u32 g_next_channel_id = 1;

bool ring_rights_valid(u32 rights) {
    return rights != 0 && (rights & ~(RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_DUP)) == 0;
}

bool doorbell_rights_valid(u32 rights) {
    return rights != 0 && (rights & ~(RIGHT_NOTIFY | RIGHT_WAIT | RIGHT_DUP)) == 0;
}

} // namespace

CapHandle create_channel(Process& caller, CapHandle vm_cap, u32 order, u32 flags,
                         SysError* out_error) {
    SysError rc = E_OK;
    Vm* v = vm_from_cap(caller, vm_cap, RIGHT_VM_CONTROL, &rc);
    if (!v) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }
    if (v->channel_count >= kMaxVmChannels) {
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    Ring* h2g = ipc::ring_create_object(caller.pid, order, flags);
    Ring* g2h = ipc::ring_create_object(caller.pid, order, flags);
    Doorbell* host_db = ipc::doorbell_create_object(caller.pid);
    Doorbell* guest_db = ipc::doorbell_create_object(caller.pid);
    if (!h2g || !g2h || !host_db || !guest_db) {
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }

    VmChannel* channel = static_cast<VmChannel*>(
        mm::heap_alloc(sizeof(VmChannel), alignof(VmChannel)));
    if (!channel) {
        if (out_error) *out_error = E_NOMEM;
        note_error();
        return kNullCap;
    }
    libk::memset(channel, 0, sizeof(*channel));
    channel->channel_id = g_next_channel_id++;
    channel->owner_vm_id = v->vm_id;
    channel->host_to_guest = h2g;
    channel->guest_to_host = g2h;
    channel->host_doorbell = host_db;
    channel->guest_doorbell = guest_db;
    channel->order = order;
    channel->flags = flags;

    const CapHandle cap_h = cap::alloc(*caller.cap_table, CAP_VM_CHANNEL, channel,
                                       RIGHT_VM_CHANNEL_SEND |
                                           RIGHT_VM_CHANNEL_RECV |
                                           RIGHT_VM_CHANNEL_NOTIFY |
                                           RIGHT_VM_CHANNEL_INSPECT |
                                           RIGHT_DUP);
    if (cap_h == kNullCap) {
        channel->dead = true;
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    v->channels[v->channel_count++] = channel;
    metric_inc(VM_DEBUG_CHANNELS_CREATED);
    if (out_error) *out_error = E_OK;
    return cap_h;
}

CapHandle channel_endpoint(Process& caller, CapHandle channel_cap, u32 endpoint_kind,
                           u32 rights, SysError* out_error) {
    u32 needed = RIGHT_VM_CHANNEL_INSPECT;
    if (endpoint_kind == VM_CHANNEL_HOST_TO_GUEST_RING && (rights & RIGHT_WRITE)) {
        needed |= RIGHT_VM_CHANNEL_SEND;
    } else if (endpoint_kind == VM_CHANNEL_GUEST_TO_HOST_RING && (rights & RIGHT_READ)) {
        needed |= RIGHT_VM_CHANNEL_RECV;
    } else if ((endpoint_kind == VM_CHANNEL_HOST_TO_GUEST_DOORBELL ||
                endpoint_kind == VM_CHANNEL_GUEST_TO_HOST_DOORBELL) &&
               (rights & RIGHT_NOTIFY)) {
        needed |= RIGHT_VM_CHANNEL_NOTIFY;
    }

    SysError rc = E_OK;
    VmChannel* channel = channel_from_cap(caller, channel_cap, needed, &rc);
    if (!channel) {
        if (out_error) *out_error = rc;
        return kNullCap;
    }

    CapHandle endpoint = kNullCap;
    switch (endpoint_kind) {
    case VM_CHANNEL_HOST_TO_GUEST_RING:
        if (!ring_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::ring_cap_alloc(caller.pid, channel->host_to_guest, rights);
        break;
    case VM_CHANNEL_GUEST_TO_HOST_RING:
        if (!ring_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::ring_cap_alloc(caller.pid, channel->guest_to_host, rights);
        break;
    case VM_CHANNEL_HOST_TO_GUEST_DOORBELL:
        if (!doorbell_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::doorbell_cap_alloc(caller.pid, channel->host_doorbell, rights);
        break;
    case VM_CHANNEL_GUEST_TO_HOST_DOORBELL:
        if (!doorbell_rights_valid(rights)) rc = E_INVALID;
        else endpoint = ipc::doorbell_cap_alloc(caller.pid, channel->guest_doorbell, rights);
        break;
    default:
        rc = E_INVALID;
        break;
    }

    if (rc != E_OK) {
        if (out_error) *out_error = rc;
        note_error();
        return kNullCap;
    }
    if (endpoint == kNullCap) {
        if (out_error) *out_error = E_OVERFLOW;
        note_error();
        return kNullCap;
    }

    if (out_error) *out_error = E_OK;
    return endpoint;
}

SysError inspect_channel(Process& caller, CapHandle channel_cap, VmChannelInfo* out) {
    if (!out) return E_INVALID;
    SysError rc = E_OK;
    VmChannel* channel = channel_from_cap(caller, channel_cap,
                                          RIGHT_VM_CHANNEL_INSPECT, &rc);
    if (!channel) return rc;

    VmChannelInfo info{};
    info.channel_id = channel->channel_id;
    info.owner_vm_id = channel->owner_vm_id;
    info.order = channel->order;
    info.flags = channel->flags;
    info.messages_host_to_guest = channel->messages_host_to_guest;
    info.messages_guest_to_host = channel->messages_guest_to_host;
    info.endpoint_present_mask =
        (channel->host_to_guest ? 1ull : 0ull) |
        (channel->guest_to_host ? 2ull : 0ull) |
        (channel->host_doorbell ? 4ull : 0ull) |
        (channel->guest_doorbell ? 8ull : 0ull);
    *out = info;
    return E_OK;
}

} // namespace vm

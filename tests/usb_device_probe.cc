// Production device executor driven by W1C / FIFO register effects. The model
// publishes the real shared-ring messages; no hardware or phone is involved.
#include "usb_device_model.h"
#include <cstdio>
int main() {
    Storage requests,replies;
    mx_u8_t payload[8192]{};
    Device device{}; Model model{};
    device.payload=libk::as_bytes(payload);
    device.link={&replies.page,&requests.page,3,1,1};
    Geometry geometry{};
    assert(decode_geometry(0x4f54310a,0,0x22affc50,0x0f80d4e8,0xfff08060,&geometry));
    assert(geometry.host_channels==16&&geometry.device_endpoints==16&&geometry.fifo_words==3968);
    assert(device.activate(model.registers(),geometry));
    assert(!(model.words[kAhbConfig/4]&kDmaEnable));
    assert(model.words[kDeviceControl/4]&(1u<<1));
    // Host traffic cannot pass while new FIFO geometry is being flushed or
    // before the disconnect interval ends. The model independently owns time.
    model.setup(18); device.poll();
    assert(model.pending()&&device.connect_phase==DeviceConnectPhase::Settling);
    host_now=149999; device.poll();
    assert(model.pending()&&(model.words[kDeviceControl/4]&(1u<<1)));
    host_now=150000; device.poll();
    assert(!model.pending()&&device.connect_phase==DeviceConnectPhase::Connected);
    assert(!(model.words[kDeviceControl/4]&(1u<<1))&&device.activity.setups==1);
    MxRingSlot slot{}; MxDeviceEvent event{};
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    model.words[kUsbConfig/4]=0x40001400; // reset/default timing, as seen on phone
    model.words[kInterruptStatus/4]|=1u<<13; device.poll();
    assert(device.activity.enumerations==1&&(model.words[kUsbConfig/4]&(15u<<10))==(9u<<10));
    device.poll(); assert(device.activity.enumerations==1); // W1C is one observation
    model.setup(18); device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    assert(event.setup.length==18);
    const auto setup_id=event.id;
    payload[512]=18; payload[513]=1;
    MxDevicePacket send{setup_id,device.generation,512,18,0,128,MxUsbTransferType::Control,0,0};
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send));
    device.poll(); assert(device.in[0].active);
    assert(device.activity.commands==1);
    device.poll(); assert(model.tx_bytes==18&&model.tx[0]==18);
    model.ack_in();
    device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_COMPLETE));
    assert(event.id==setup_id&&event.endpoint==128&&event.length==18);
    assert(device.activity.in_complete==1);
    // A newer SETUP invalidates late replies and never lets their bytes start.
    model.setup(8); device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    assert(event.id>setup_id);
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send)); device.poll();
    assert(!device.in[0].active);
    // An aborting SETUP fences old IN bytes before a new setup can be offered.
    send.id=event.id; send.generation=device.generation;
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send)); device.poll();
    assert(device.in[0].active);
    model.setup(8); device.poll();
    assert(!device.in[0].active&&!device.setup_flushing);
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    // Bus reset invalidates endpoint work even if an old completion is pending.
    model.words[kInterruptStatus/4]|=1u<<12; device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_RESET));
    assert(event.generation>send.generation&&!device.in[0].active);
    assert(device.activity.resets==1&&device.activity.setups==4);
    // A stuck hardware flush never exposes the pull-up, even at timeout.
    Device blocked{}; Model stuck{};
    blocked.payload=libk::as_bytes(payload); stuck.hold_flush=true;
    assert(blocked.activate(stuck.registers(),geometry));
    assert(!blocked.advance_connect(host_now+100000000ull));
    assert(blocked.connect_phase==DeviceConnectPhase::Fault&&!blocked.active);
    assert(stuck.words[kDeviceControl/4]&(1u<<1));
    std::puts("usb device probe ok");
}

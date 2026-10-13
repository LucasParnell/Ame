// Production device executor driven by W1C / FIFO register effects. The model
// publishes the real shared-ring messages; no hardware or phone is involved.
#include "../userland/drivers/usb/dwc2/device.h"
#include <cassert>
#include <cstdio>
using namespace mochi::usb::dwc2;
struct Storage { MxRingPage page{}; MxRingSlot slots[8]{}; };
struct Model {
    mx_u32_t words[1024]{};
    mx_u8_t rx[512]{},tx[512]{};
    mx_u32_t status,bytes,tx_bytes;
    bool pending;
    Registers registers() {
        return {this,
            [](void* p,mx_u32_t reg,mx_u32_t* out) {
                auto& m=*static_cast<Model*>(p);
                if(reg>=sizeof(m.words)) return false;
                *out=m.words[reg/4];
                if(reg==kInterruptStatus) *out|=m.pending?kRxAvailable:0;
                if(reg==kRxStatusPop) { *out=m.status; m.pending=false; }
                if(reg>=kInEndpointBase&&reg<kOutEndpointBase&&(reg-kInEndpointBase)%32==0x18) *out=128;
                return true;
            },
            [](void* p,mx_u32_t reg,mx_u32_t value) {
                auto& m=*static_cast<Model*>(p);
                if(reg>=sizeof(m.words)) return false;
                const bool endpoint_irq=reg>=kInEndpointBase&&(reg&31)==8;
                if(reg==kInterruptStatus||endpoint_irq) m.words[reg/4]&=~value;
                else if(reg==kReset) m.words[reg/4]=kAhbIdle; // flush finishes immediately
                else m.words[reg/4]=value;
                return true;
            },
            [](void* p,mx_u32_t reg,libk::bytes destination) {
                auto& m=*static_cast<Model*>(p);
                return reg==fifo_register(0)&&destination.size==m.bytes&&libk::copy_n(destination,libk::as_bytes(m.rx),destination.size);
            },
            [](void* p,mx_u32_t reg,libk::const_bytes source) {
                auto& m=*static_cast<Model*>(p);
                m.tx_bytes=source.size;
                return reg>=kFifoBase&&libk::copy_n(libk::as_bytes(m.tx),source,source.size);
            },0x40000};
    }
    void setup(mx_u16_t length) {
        const mx_u8_t packet[]={0x80,6,0,1,0,0,static_cast<mx_u8_t>(length),0};
        libk::copy_n(libk::as_bytes(rx),libk::as_bytes(packet),8);
        status=(8u<<4)|(6u<<17); bytes=8; pending=true;
    }
};
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
    model.setup(18); device.poll();
    MxRingSlot slot{}; MxDeviceEvent event{};
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_SETUP));
    assert(event.setup.length==18);
    const auto setup_id=event.id;
    payload[512]=18; payload[513]=1;
    MxDevicePacket send{setup_id,device.generation,512,18,0,128,MxUsbTransferType::Control,0,0};
    assert(device.link.rx().try_push_msg(MX_DEVICE_SEND,send));
    device.poll(); assert(device.in[0].active);
    device.poll(); assert(model.tx_bytes==18&&model.tx[0]==18);
    model.words[Device::reg(true,0,8)/4]|=1;
    device.poll();
    assert(device.link.tx().try_pop(&slot)&&mochi::message_get(slot,&event,MX_DEVICE_COMPLETE));
    assert(event.id==setup_id&&event.endpoint==128&&event.length==18);
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
    std::puts("usb device probe ok");
}

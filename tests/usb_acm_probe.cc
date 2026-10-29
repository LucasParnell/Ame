// Exercise production ACM control while both byte directions are blocked.
#include "../userland/usb/functions/acm/bridge.h"
#include <cassert>
#include <cstdio>
struct Storage { MxRingPage page{}; MxRingSlot slots[8]{}; };
int main() {
    Storage outgoing[3],incoming[3];
    mochi::usb::acm::Runtime r{};
    for(unsigned i=0;i<3;i++)r.links[i]={&outgoing[i].page,&incoming[i].page,3,1,1};
    r.control.init();
    MxFunctionChunk bytes{7,1,32,0,0,{}};
    for(unsigned i=0;i<8;i++) {
        assert(r.links[0].tx().try_push_msg(MX_FUNCTION_TX,bytes));
        assert(r.links[1].tx().try_push_msg(MX_FUNCTION_RX,bytes));
    }
    assert(r.links[0].rx().try_push_msg(MX_FUNCTION_RX,bytes));
    assert(r.links[1].rx().try_push_msg(MX_FUNCTION_TX,bytes));
    assert(mochi::usb::acm::service(r));
    assert(r.pending[0].held && r.pending[1].held);
    MxFunctionSetup request{};
    request.id=3;request.generation=7;request.setup={0x21,0x22,3,0,0};
    assert(r.links[2].rx().try_push_msg(MX_FUNCTION_SETUP,request));
    assert(mochi::usb::acm::service(r));
    assert(mochi::usb::acm::service(r));
    MxRingSlot slot{};
    MxFunctionSetup reply{};
    assert(r.links[2].tx().try_pop(&slot));
    assert(mochi::message_get(slot,&reply,MX_FUNCTION_SETUP_REPLY));
    assert(reply.id==3 && reply.generation==7 && reply.status==MX_OK && !reply.length);
    assert(r.control.lines==3 && r.pending[0].held && r.pending[1].held);
    assert(r.links[0].tx().depth()==8 && r.links[1].tx().depth()==8);
    // The reply itself remains owned while the independent control ring is full.
    for(unsigned i=0;i<8;i++)assert(r.links[2].tx().try_push_msg(MX_FUNCTION_SETUP_REPLY,reply));
    request.id=4;
    assert(r.links[2].rx().try_push_msg(MX_FUNCTION_SETUP,request));
    assert(mochi::usb::acm::service(r));
    assert(!mochi::usb::acm::service(r));
    assert(r.control_reply.held);
    assert(r.links[2].tx().try_pop(&slot));
    assert(mochi::usb::acm::service(r));
    assert(!r.control_reply.held);
    std::puts("usb acm control independence probe ok");
}

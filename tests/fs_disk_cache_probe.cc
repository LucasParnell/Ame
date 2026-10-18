#include <cassert>
#include <cstring>
#include "../userland/fs/fs_core.cc"
#include "../userland/fs/fs_handles.cc"
#include "../userland/fs/fs_snapshot.cc"
#include "../userland/fs/fs_durable.cc"
#include "../userland/fs/fs_disk.cc"

struct Pipe { RingPage page; RingSlot slots[2]; } requests{},replies{};
alignas(4096) static mx_u8_t transfer[131072], media[64][4096];
static unsigned operations;
static bool partial,stall;
static void device() {
    host_now+=100000000;
    if(stall)return;
    RingSlot slot{};
    if(!ring_pop(&requests.page,1,&slot))return;
    ++operations;
    BlkRequest r{};assert(mochi::message_get(slot,&r));
    assert(!r.arena_cap);
    const auto bytes=static_cast<mx_size_t>(r.sector_count)*512;
    assert(r.sector*512+bytes<=sizeof(media));
    auto* data=reinterpret_cast<mx_u8_t*>(media)+r.sector*512;
    auto* arena=transfer+r.arena_offset;
    mx_i32_t status=MX_OK;mx_u32_t done=r.sector_count;
    if(slot.type==BLK_READ)std::memcpy(arena,data,bytes);
    else if(slot.type==BLK_WRITE) {
        std::memcpy(data,arena,partial?bytes/2:bytes);
        if(partial){status=MX_ERR_FAULT;done=r.sector_count/2;partial=false;}
    }else assert(slot.type==BLK_FLUSH);
    BlkReply response{status,done};
    assert(ring_try_push(&replies.page,1,BLK_REPLY,&response,sizeof(response),1));
}
int main() {
    g_blk_client.ep={&requests.page,&replies.page,1,1,1};
    g_blk_available=true;g_blk_dma=transfer;g_blk_dma_size=sizeof(transfer);
    g_disk_total_blocks=64;g_disk_partition_sectors=64*8;
    host_on_yield=device;
    std::memset(media[10],0x11,4096);
    mx_u8_t result[4096],value[4096];std::memset(value,0x82,sizeof(value));
    assert(disk_read_block(10,result)&&result[0]==0x11);
    const auto before=operations;
    assert(disk_read_block(10,result)&&operations==before);
    partial=true;assert(!disk_write_block(10,value));
    assert(disk_read_block(10,result)&&result[0]==0x82&&result[4095]==0x11);
    assert(operations==before+2); // partial write invalidates the old cache row
    assert(disk_write_block(10,value));
    const auto complete=operations;
    assert(disk_read_block(10,result)&&result[4095]==0x82&&operations==complete);
    assert(!disk_write_blocks(~mx_u64_t{0},2,value));
    assert(!disk_read_blocks(63,2,result));
    stall=true;assert(!disk_write_block(10,value));assert(!g_blk_available);
    std::memset(value,0x44,sizeof(value));
    mx_u8_t previous[4096];std::memcpy(previous,transfer+BLK_DMA_CLIENT_OFFSET,4096);
    assert(!disk_write_block(10,value));
    assert(!std::memcmp(previous,transfer+BLK_DMA_CLIENT_OFFSET,4096));
    stall=false;device(); // a late acknowledgement cannot revive the poisoned transport
    assert(!disk_read_block(10,result));
}

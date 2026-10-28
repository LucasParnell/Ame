#pragma once
#include "wire.h"
namespace ame::vk {
constexpr uint32_t MemoryMap=0x10001,MemoryUnmap=0x10002,MemoryUpload=0x10003,MemoryDownload=0x10004;
struct MemoryRequest {
    Header header;
    uint64_t device,memory,offset,size;
    uint32_t flags,reserved;
};
constexpr uint32_t MemoryPayload=AME_VK_WIRE_BYTES-sizeof(MemoryRequest);
}

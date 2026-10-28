#pragma once
#include "wire.h"
namespace ame::vk {
constexpr uint32_t CTSStart=0x10006,CTSPoll=0x10007,CTSLog=0x10008;
struct CTSRequest {
    Header header;
    uint32_t running,exit_status,log_offset,log_bytes;
    char pattern[256];
};
constexpr uint32_t CTSLogPayload=AME_VK_WIRE_BYTES-sizeof(CTSRequest);
}

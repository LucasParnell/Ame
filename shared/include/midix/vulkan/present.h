#pragma once
#include "wire.h"
namespace ame::vk {
constexpr uint32_t Present=0x10005;
struct PresentRequest {Header header;uint64_t device,queue,image;uint32_t finish,reserved;};
}

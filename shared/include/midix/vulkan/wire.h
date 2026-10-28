#pragma once
#include <stdint.h>
#include <stddef.h>
/* All addresses in a request are checked byte offsets into this packet.
 * Native and guest both use the Vulkan AArch64 LP64 ABI. No guest pointers or
 * Vulkan driver handles are exported. Responses retire with the transport ACK. */
#define AME_VK_WIRE_VERSION 1u
#define AME_VK_WIRE_BYTES 16384u
#define AME_VK_MAX_FIXUPS 1024u
#define AME_VK_MAX_HANDLES 4096u
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Ame Vulkan wire requires little-endian LP64"
#endif
namespace ame::vk {
static_assert(sizeof(void*)==8,"Ame Vulkan wire requires LP64 pointers");
enum class Kind : uint8_t { Plain, Handle, Structure, Pointer, String, Unsupported };
struct Field {
    uint32_t offset, type;
    uint64_t (*count)(const void*);
    bool input, output;
};
struct Type {
    uint32_t bytes;
    Kind kind;
    const Field* fields;
    uint32_t field_count;
};
struct Header {
    uint32_t version, command, bytes, root;
    int32_t result;
    uint32_t reserved;
};
struct Copy { void* target; uint32_t offset, bytes; };
/* Metadata is generated from the pinned Khronos registry. */
const Type& type(uint32_t id);
struct Encoder {
    alignas(16) uint8_t data[AME_VK_WIRE_BYTES]{};
    Copy copies[AME_VK_MAX_FIXUPS]{};
    uint32_t used=sizeof(Header), copy_count=0;
    bool good=true;
    uint32_t object(uint32_t id, const void* source, uint64_t count,
                    bool input, bool output, unsigned depth=0);
    void walk(uint32_t id,uint32_t start,const void* source,uint64_t count,bool input,bool output,unsigned depth);
    void finish();
    void copy_back();
};
struct Handle {
    uint64_t actual, token;
    uint32_t type;
    uint64_t owner=0;
    bool dead=false;
};
struct Handles {
    Handle entries[AME_VK_MAX_HANDLES]{};
    uint64_t sequence=0;
    uint64_t actual(uint32_t type, uint64_t token, bool& good);
    uint64_t token(uint32_t type, uint64_t actual, bool& good);
    void erase(uint32_t type, uint64_t actual);
    void erase_children(uint32_t type,uint64_t actual);
    void commit();
};
struct Fixup { uint32_t field, offset, type, count; bool output; };
struct Decoder {
    uint8_t* data;
    uint32_t bytes;
    Handles& handles;
    Fixup fixups[AME_VK_MAX_FIXUPS]{};
    uint32_t fixup_count=0;
    struct Region {uint32_t offset,bytes;} regions[AME_VK_MAX_FIXUPS]{};
    uint32_t region_count=0;
    bool finished=false;
    bool reserve_outputs();
    bool region(uint64_t offset,uint64_t size);
    bool good=true;
    void object(uint32_t id, uint32_t offset, uint64_t count,
                bool input, bool output, unsigned depth=0);
    void finish();
    void parent_outputs(uint64_t parent);
};
} // namespace ame::vk

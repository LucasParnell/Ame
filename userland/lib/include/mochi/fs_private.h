#pragma once
#include "mochi/message.h"
#include "midix/abi/protocols/fs.h"
namespace mochi::fs {
inline bool private_state_name(const char* name,mx_u32_t size) {
    static constexpr char key[]=".fido2";
    if (size!=sizeof(key)-1) return false;
    for(mx_u32_t i=0;i<size;++i) if(name[i]!=key[i]) return false;
    return true;
}
inline bool private_state_request(const MxRingSlot& slot) {
    if (slot.type==FS_OPEN) {FsOpenRequest r{};return message_get(slot,&r,FS_OPEN) && private_state_name(r.name,r.name_len);}
    if (slot.type==FS_MKDIR) {FsMkdirRequest r{};return message_get(slot,&r,FS_MKDIR) && private_state_name(r.name,r.name_len);}
    if (slot.type==FS_UNLINK) {FsUnlinkRequest r{};return message_get(slot,&r,FS_UNLINK) && private_state_name(r.name,r.name_len);}
    if (slot.type==FS_RENAME) {FsRenameRequest r{};return message_get(slot,&r,FS_RENAME) &&
        (private_state_name(r.old_name,r.old_name_len) || private_state_name(r.new_name,r.new_name_len));}
    return false;
}
} // namespace mochi::fs

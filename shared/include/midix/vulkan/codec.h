#pragma once
#include "wire.h"
namespace ame::vk {
inline void bytes_copy(void* to, const void* from, uint32_t n) {
    auto* d=static_cast<uint8_t*>(to);const auto* s=static_cast<const uint8_t*>(from);
    for(uint32_t i=0;i<n;i++)d[i]=s[i];
}
inline uint64_t load64(const void* p) {uint64_t v;bytes_copy(&v,p,8);return v;}
inline void store64(void* p,uint64_t v) {bytes_copy(p,&v,8);}
inline uint32_t Encoder::object(uint32_t id,const void* source,uint64_t count,
                                 bool input,bool output,unsigned depth) {
    if (!good || !count || !source) return 0;
    const Type& t=type(id);
    if(depth>32 || t.kind==Kind::Unsupported || !t.bytes || count>AME_VK_WIRE_BYTES/t.bytes) {good=false;return 0;}
    uint32_t n=static_cast<uint32_t>(count)*t.bytes;
    uint32_t start=(used+7)&~7u;
    if(start>AME_VK_WIRE_BYTES || n>AME_VK_WIRE_BYTES-start) {good=false;return 0;}
    used=start+n;bytes_copy(data+start,source,n);
    if(output) {
        if(copy_count==AME_VK_MAX_FIXUPS) {good=false;return 0;}
        copies[copy_count++]={const_cast<void*>(source),start,n};
    }
    walk(id,start,source,count,input,output,depth);
    return start;
}
inline void Encoder::walk(uint32_t id,uint32_t start,const void* source,uint64_t count,
                          bool input,bool output,unsigned depth) {
    if(depth>32){good=false;return;}
    const Type& t=type(id);
    for(uint32_t i=0;i<count;i++) {
        const auto* s=static_cast<const uint8_t*>(source)+i*t.bytes;
        auto* d=data+start+i*t.bytes;
        if(t.kind==Kind::Structure) {
            for(uint32_t f=0;f<t.field_count;f++) {
                const Field& field=t.fields[f];
                const Type& child=type(field.type);
                if(child.kind==Kind::Pointer || child.kind==Kind::String || child.kind==Kind::Unsupported) {
                    uint64_t ptr=load64(s+field.offset);
                    uint64_t child_count=field.count(s);
                    uint32_t off=0;
                    if(ptr && child_count) {
                        if(child.kind==Kind::Unsupported) {good=false;return;}
                        if(child.kind==Kind::String) {
                            const auto* str=reinterpret_cast<const char*>(ptr);
                            child_count=0;
                            while(child_count<AME_VK_WIRE_BYTES && str[child_count])child_count++;
                            if(child_count==AME_VK_WIRE_BYTES) {good=false;return;}
                            child_count++;
                        }
                        off=object(child.fields[0].type,reinterpret_cast<const void*>(ptr),child_count,
                            input&&field.input,output||field.output,depth+1);
                    }
                    store64(d+field.offset,off);
                } else if(child.kind==Kind::Structure) {
                    walk(field.type,start+i*t.bytes+field.offset,s+field.offset,
                         field.count(s),input,output,depth+1);
                    if(!good)return;
                }
            }
        } else if(t.kind==Kind::Pointer || t.kind==Kind::String) {
            uint64_t ptr=load64(s), child_count=1;
            if(t.kind==Kind::String && ptr) {
                const char* str=reinterpret_cast<const char*>(ptr);child_count=0;
                while(child_count<AME_VK_WIRE_BYTES && str[child_count])child_count++;
                if(child_count==AME_VK_WIRE_BYTES) {good=false;return;}child_count++;
            }
            store64(d,object(t.fields[0].type,reinterpret_cast<const void*>(ptr),child_count,input,output,depth+1));
        }
    }
}
inline void Encoder::finish() {reinterpret_cast<Header*>(data)->bytes=used;}
inline void Encoder::copy_back() {
    if(!good)return;
    for(uint32_t i=0;i<copy_count;i++)bytes_copy(copies[i].target,data+copies[i].offset,copies[i].bytes);
}
inline uint64_t Handles::actual(uint32_t id,uint64_t token_value,bool& good) {
    if(!token_value)return 0;
    for(const auto& e:entries)if(e.token==token_value&&e.type==id&&!e.dead)return e.actual;
    good=false;return 0;
}
inline uint64_t Handles::token(uint32_t id,uint64_t value,bool& good) {
    if(!value)return 0;
    for(const auto& e:entries)if(e.actual==value&&e.type==id)return e.token;
    for(auto& e:entries)if(!e.token) {
        if(sequence==UINT64_MAX)break;
        e={value,++sequence,id};return e.token;
    }
    good=false;return 0;
}
inline void Handles::erase(uint32_t id,uint64_t value) {
    for(auto& e:entries)if(e.type==id&&e.actual==value)e.dead=true;
}
inline void Handles::erase_children(uint32_t id,uint64_t value) {
    uint64_t parent=0;
    for(const auto& e:entries)if(e.token && e.type==id && e.actual==value)parent=e.token;
    if(parent)for(auto& e:entries)if(e.token && e.owner==parent)e.dead=true;
}
inline void Handles::commit() {
    bool changed;
    do {changed=false;
        for(const auto& parent:entries)if(parent.token && parent.dead)
            for(auto& child:entries)if(child.token && !child.dead && child.owner==parent.token) {child.dead=true;changed=true;}
    } while(changed);
    for(auto& e:entries)if(e.dead)e={};
}
inline void Decoder::parent_outputs(uint64_t parent) {
    for(uint32_t i=0;i<fixup_count;i++) {
        const auto& f=fixups[i];
        if(type(f.type).kind!=Kind::Handle || !f.output)continue;
        uint64_t token_value=load64(data+f.field);
        for(auto& e:handles.entries)if(e.token==token_value && token_value)e.owner=parent;
    }
}
inline bool Decoder::region(uint64_t off,uint64_t size) {
    if(!good || off<sizeof(Header) || off>bytes || size>bytes-off || (off&7) || region_count==AME_VK_MAX_FIXUPS) {good=false;return false;}
    for(uint32_t i=0;i<region_count;i++) {
        const auto& r=regions[i];
        if(off<r.offset+r.bytes && r.offset<off+size) {good=false;return false;}
    }
    regions[region_count++]={static_cast<uint32_t>(off),static_cast<uint32_t>(size)};return true;
}
inline void Decoder::object(uint32_t id,uint32_t offset,uint64_t count,bool input,bool output,unsigned depth) {
    const Type& t=type(id);
    if(!good || !count)return;
    if(depth>32 || t.kind==Kind::Unsupported || !t.bytes || count>bytes/t.bytes ||
       offset<sizeof(Header) || offset>bytes || count*t.bytes>bytes-offset) {good=false;return;}
    for(uint32_t i=0;i<count;i++) {
        uint32_t pos=offset+i*t.bytes;auto* p=data+pos;
        if(t.kind==Kind::Handle) {
            if(input)store64(p,handles.actual(id,load64(p),good));
            else store64(p,0);
            /* Output handles are translated after the driver call. */
            if(fixup_count==AME_VK_MAX_FIXUPS){good=false;return;}
            fixups[fixup_count++]={pos,0,id,0,output};
        } else if(t.kind==Kind::Structure) {
            for(uint32_t f=0;f<t.field_count;f++) {
                const Field& field=t.fields[f];const Type& child=type(field.type);
                if(child.kind==Kind::Pointer || child.kind==Kind::String || child.kind==Kind::Unsupported) {
                    uint64_t off=load64(p+field.offset), n=field.count(p);
                    if(!off || !n) {store64(p+field.offset,0);continue;}
                    if(child.kind==Kind::Unsupported || off>UINT32_MAX || fixup_count==AME_VK_MAX_FIXUPS) {good=false;return;}
                    if(child.kind==Kind::String) {
                        n=0;while(off+n<bytes && data[off+n])n++;
                        if(off+n>=bytes){good=false;return;}n++;
                    }
                    if(n>bytes/type(child.fields[0].type).bytes || !region(off,n*type(child.fields[0].type).bytes))return;
                    object(child.fields[0].type,off,n,input&&field.input,output||field.output,depth+1);
                    if(!good)return;
                    fixups[fixup_count++]={pos+field.offset,static_cast<uint32_t>(off),field.type,static_cast<uint32_t>(n),false};
                    store64(p+field.offset,reinterpret_cast<uint64_t>(data+off));
                } else {
                    object(field.type,pos+field.offset,field.count(p),input,output,depth+1);
                }
            }
        } else if(t.kind==Kind::Pointer || t.kind==Kind::String) {
            uint64_t off=load64(p),n=1;
            if(!off)continue;
            if(off>UINT32_MAX || fixup_count==AME_VK_MAX_FIXUPS){good=false;return;}
            if(t.kind==Kind::String) {
                n=0;while(off+n<bytes && data[off+n])n++;
                if(off+n>=bytes){good=false;return;}n++;
            }
            if(n>bytes/type(t.fields[0].type).bytes || !region(off,n*type(t.fields[0].type).bytes))return;
            object(t.fields[0].type,off,n,input,output,depth+1);
            if(!good)return;
            fixups[fixup_count++]={pos,static_cast<uint32_t>(off),id,static_cast<uint32_t>(n),false};
            store64(p,reinterpret_cast<uint64_t>(data+off));
        }
    }
}
inline bool Decoder::reserve_outputs() {
    unsigned available=0,needed=0;
    for(const auto& e:handles.entries)if(!e.token)available++;
    for(uint32_t i=0;i<fixup_count;i++)if(type(fixups[i].type).kind==Kind::Handle && fixups[i].output)needed++;
    return needed<=available;
}
inline void Decoder::finish() {
    if(finished)return;
    finished=true;
    /* Restore children before pointer fields so no guest VA escapes. */
    for(uint32_t i=0;i<fixup_count;i++) {
        const auto& f=fixups[i];
        if(type(f.type).kind==Kind::Handle)store64(data+f.field,handles.token(f.type,load64(data+f.field),good));
        else store64(data+f.field,f.offset);
    }
}
} // namespace ame::vk

#!/usr/bin/env python3
"""Generate Ame's native Vulkan 1.0 RPC from the pinned Khronos registry.

No struct layout, count expression or function signature is handwritten in the
wire ABI. Unsupported callback/pNext chains fail before a driver call.
"""
from pathlib import Path
import argparse
import re
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
r=ET.parse(ROOT/'third_party/vulkan/vk.xml').getroot()
types={t.get('name',t.findtext('name')):t for t in r.findall('./types/type')}
commands={c.findtext('proto/name'):c for c in r.findall('./commands/command') if c.find('proto') is not None}
feature=next(f for f in r.findall('feature') if f.get('number')=='1.0')
names=[c.get('name') for q in feature.findall('require') if 'vulkan' in q.get('api','vulkan').split(',') for c in q.findall('command')]
# Host VA mappings need a dedicated bounded upload/download implementation.
manual={'vkGetInstanceProcAddr','vkGetDeviceProcAddr','vkMapMemory','vkUnmapMemory',
        'vkFlushMappedMemoryRanges','vkInvalidateMappedMemoryRanges'}
entries={};roots={};callbacks=[]
def add(name,kind,fields=None,ctype=None):
    if name in entries:return name
    entries[name]={'kind':kind,'fields':fields or [],'ctype':ctype or name};return name

def base(name):
    if name in entries:return name
    t=types.get(name)
    if t is None:return add(name,'Plain')
    cat=t.get('category')
    if t.get('alias'):return base(t.get('alias'))
    if cat=='handle':return add(name,'Handle')
    if cat=='funcpointer' or name=='VkAllocationCallbacks':return add(name,'Unsupported')
    if cat not in ('struct',):return add(name,'Plain')
    add(name,'Structure')
    entries[name]['fields']=[member(m,name) for m in t.findall('member') if 'vulkan' in m.get('api','vulkan').split(',')]
    return name

def member(m,parent):
    name=m.findtext('name');typename=m.findtext('type');decl=''.join(m.itertext()).strip()
    # Comments are separate XML children and must not enter a C signature.
    decl=''.join((m.text or '',*(ET.tostring(c,encoding='unicode',method='text') if c.tag!='comment' else '' for c in m)))
    decl=decl.strip();prefix=decl[:decl.find(name)]
    depth=prefix.count('*');const='const' in prefix
    if parent.startswith('Args_') and '[' in decl and not depth:
        size=re.search(r'\[([^]]+)\]',decl)[1]
        decl=decl[:decl.index('[')].replace(name,'*'+name)
        depth=1;length_override=size
    else:length_override=None
    length=length_override or m.get('len','1')
    # Union, scalar arrays and struct arrays are contiguous by-value members.
    inline_count='1'
    if not depth:
        match=re.search(r'\[([^]]+)\]',decl)
        if match:inline_count=match[1]
    child=base(typename) if typename!='void' else add('byte','Plain',ctype='uint8_t')
    if depth:
        if name=='pNext' or typename=='VkAllocationCallbacks' or depth>2:
            child=add('unsupported_pointer','Unsupported',ctype='void*')
        else:
            if depth==2:
                child=add('string_pointer' if typename=='char' else f'ptr_{child}',
                    'String' if typename=='char' else 'Pointer',[{'type':child}],ctype='void*')
            child=add(f'pointer_{child}_{depth}_{"s" if length=="null-terminated" else "a"}',
                'String' if length=='null-terminated' and depth==1 else 'Pointer',[{'type':child}],ctype='void*')
    if not depth:expr=inline_count
    elif length=='null-terminated':expr='1'
    elif 'latexmath' in length:
        if name=='pCode':expr='s->codeSize / 4'
        elif name=='pSampleMask':expr='(static_cast<uint64_t>(s->rasterizationSamples)+31)/32'
        else:raise ValueError((parent,name,length))
    else:
        length=length.split(',')[0]
        expr=length
        for token in sorted(set(re.findall(r'\b[A-Za-z_]\w*\b',length)),key=len,reverse=True):
            if token.isupper():continue
            # pAllocateInfo->descriptorSetCount is rooted in the first token.
            if '->'+token in length:continue
            expr=re.sub(r'\b'+token+r'\b','s->'+token,expr)
        if re.match(r'p[A-Z]',length) and '->' not in length:
            expr=f'(s->{length} ? *s->{length} : 0)'
        if '->' in length:
            first=length.split('->')[0];expr=f'(s->{first} ? {expr} : 0)'
    # Fields ignored by Vulkan must never dereference an unused client pointer.
    if parent=='VkWriteDescriptorSet':
        cond={'pImageInfo':'s->descriptorType<=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE',
              'pTexelBufferView':'s->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || s->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER',
              'pBufferInfo':'s->descriptorType>=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && s->descriptorType<=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC'}.get(name)
        if cond:expr=f'(({cond}) ? {expr} : 0)'
    if name=='pQueueFamilyIndices':expr=f'(s->sharingMode==VK_SHARING_MODE_CONCURRENT ? {expr} : 0)'
    return {'name':name,'type':child,'count':expr,'input':not(depth and not const),'output':bool(depth and not const),'decl':decl}

for name in names:
    if name in manual:continue
    root='Args_'+name
    add(root,'Structure')
    entries[root]['fields']=[member(m,root) for m in commands[name].findall('param')]
    roots[name]=root
ids={name:i for i,name in enumerate(entries)}
prologue='// Generated by scripts/vulkan/generate-frontend.py; do not edit.\n'
h=[prologue,'#pragma once\n#include <vulkan/vulkan.h>\n#include "midix/vulkan/wire.h"\nnamespace ame::vk {\n']
for name,root in roots.items():
    h.append('struct '+root+' {\n'+''.join('    '+f['decl']+';\n' for f in entries[root]['fields'])+'};\n')
for n,i in ids.items():h.append(f'constexpr uint32_t T_{n}={i};\n')
for i,n in enumerate(names):h.append(f'constexpr uint32_t C_{n}={i+1};\n')
h.append('} // namespace ame::vk\n');(a.output/'vk-rpc-types.h').write_text(''.join(h))
meta=[prologue,'#include "vk-rpc-types.h"\nnamespace ame::vk {\n']
for name,e in entries.items():
    fs=e['fields']
    if not fs:continue
    for i,f in enumerate(fs):
        if 'name' not in f:continue
        meta.append(f'static uint64_t count_{ids[name]}_{i}(const void* p) {{ const auto* s=static_cast<const {name}*>(p); (void)s; return {f["count"]}; }}\n')
    meta.append(f'static const Field fields_{ids[name]}[]={{\n')
    for i,f in enumerate(fs):
        if 'name' in f:meta.append(f'    {{offsetof({name},{f["name"]}),T_{f["type"]},count_{ids[name]}_{i},{str(f["input"]).lower()},{str(f["output"]).lower()}}},\n')
        else:meta.append(f'    {{0,T_{f["type"]},nullptr,true,false}},\n')
    meta.append('};\n')
meta.append('static const Type types[]={\n')
for name,e in entries.items():
    fs=e['fields'];meta.append(f'    {{sizeof({e["ctype"]}),Kind::{e["kind"]},{"fields_"+str(ids[name]) if fs else "nullptr"},{len(fs)}}}, // {name}\n')
meta.append('};\nconst Type& type(uint32_t id) { return types[id]; }\n} // namespace ame::vk\n')
(a.output/'vk-rpc-metadata.cc').write_text(''.join(meta))
client=[prologue,'#include "vk-rpc-types.h"\n#include "client.h"\nusing namespace ame::vk;\nextern "C" {\n']
guest=[prologue,'#include "vk-rpc-types.h"\nusing namespace ame::vk;\nstatic VkResult dispatch(Decoder& decoder, uint32_t command, uint32_t root) {\n switch(command) {\n']
for name,root in roots.items():
    fs=entries[root]['fields'];ret=commands[name].findtext('proto/type');params=', '.join(f['decl'] for f in fs)
    values=', '.join(f['name'] for f in fs)
    client.append(f'VKAPI_ATTR {ret} VKAPI_CALL {name}({params}) {{\n    {root} args{{{values}}};\n')
    call=f'call(C_{name},T_{root},&args)'
    if name=='vkFreeMemory':client.append('    forget_mapping(memory);\n')
    if name.startswith('vkCmd'):
        client.append(f'    record_error(commandBuffer,{call});\n}}\n')
    elif name=='vkEndCommandBuffer':
        client.append(f'    VkResult result={call};\n    VkResult pending=record_status(commandBuffer);return pending!=VK_SUCCESS ? pending : result;\n}}\n')
    else:
        if name in ('vkBeginCommandBuffer','vkResetCommandBuffer'):client.append('    record_clear(commandBuffer);\n')
        client.append(f'    {"return " if ret!="void" else "(void)"}{call};\n}}\n')
    guest.append(f' case C_{name}: {{\n    if(!decoder.region(root,sizeof({root})))return VK_ERROR_INITIALIZATION_FAILED;\n    decoder.object(T_{root},root,1,true,false);\n    if(!decoder.good)return VK_ERROR_INITIALIZATION_FAILED;\n    if(!decoder.reserve_outputs())return VK_ERROR_OUT_OF_HOST_MEMORY;\n    auto& a=*reinterpret_cast<{root}*>(decoder.data+root);\n')
    # Driver dispatch tables are looked up through the correct instance/device.
    first=fs[0]['name'] if fs else ''
    first_type=commands[name].find('param/type').text if fs else ''
    scope='instance_proc'
    if first_type in ('VkDevice','VkQueue','VkCommandBuffer'):scope='device_proc'
    guest.append(f'    auto fn=reinterpret_cast<PFN_{name}>({scope}("{name}"));\n    if(!fn){{decoder.finish();return VK_ERROR_FEATURE_NOT_PRESENT;}}\n')
    args=', '.join('a.'+f['name'] for f in fs)
    if name.startswith('vkEnumerate') and ('ExtensionProperties' in name or 'LayerProperties' in name):
        guest.append('    *a.pPropertyCount=0;\n    VkResult result=VK_SUCCESS;\n')
    else:
        if name=='vkCreateInstance':guest.append('    if(a.pCreateInfo->enabledLayerCount || a.pCreateInfo->enabledExtensionCount || (a.pCreateInfo->pApplicationInfo && (a.pCreateInfo->pApplicationInfo->apiVersion & ~0xfffu)>VK_API_VERSION_1_0)) {decoder.finish();return VK_ERROR_INCOMPATIBLE_DRIVER;}\n')
        if name=='vkCreateDevice':guest.append('    if(a.pCreateInfo->enabledLayerCount || a.pCreateInfo->enabledExtensionCount) {decoder.finish();return VK_ERROR_EXTENSION_NOT_PRESENT;}\n')
        guest.append(f'    {"VkResult result=" if ret=="VkResult" else ""}fn({args});\n')
    if name=='vkGetPhysicalDeviceProperties':guest.append('    a.pProperties->apiVersion=VK_API_VERSION_1_0;\n')
    if name=='vkGetPhysicalDeviceMemoryProperties':guest.append('    for(uint32_t i=0;i<a.pMemoryProperties->memoryTypeCount;i++)a.pMemoryProperties->memoryTypes[i].propertyFlags &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;\n')
    retire=None
    if name.startswith('vkDestroy') or name=='vkFreeMemory':
        idx=0 if name in ('vkDestroyInstance','vkDestroyDevice') else 1
        retire=fs[idx]['name']
        guest.append(f'    uint64_t retired=reinterpret_cast<uint64_t>(a.{retire});\n')
    if name=='vkCreateImage':guest.append('    if(result==VK_SUCCESS && !remote_image_created(a.device,*a.pImage,a.pCreateInfo)) {auto destroy=reinterpret_cast<PFN_vkDestroyImage>(device_proc("vkDestroyImage"));destroy(a.device,*a.pImage,nullptr);*a.pImage=VK_NULL_HANDLE;result=VK_ERROR_OUT_OF_HOST_MEMORY;}\n')
    if name=='vkBindImageMemory':guest.append('    if(result==VK_SUCCESS)remote_image_bound(a.image,a.memory,a.memoryOffset);\n')
    if name=='vkDestroyImage':guest.append('    remote_image_destroyed(a.image);\n')
    if name=='vkAllocateMemory':guest.append('    if(result==VK_SUCCESS && !remote_allocated(a.device,*a.pMemory,a.pAllocateInfo->allocationSize)) {auto free_fn=reinterpret_cast<PFN_vkFreeMemory>(device_proc("vkFreeMemory"));free_fn(a.device,*a.pMemory,nullptr);*a.pMemory=VK_NULL_HANDLE;result=VK_ERROR_OUT_OF_HOST_MEMORY;}\n')
    if name=='vkFreeMemory':guest.append('    remote_freeing(a.memory);\n')
    if name in ('vkFreeCommandBuffers','vkFreeDescriptorSets'):
        kind='VkCommandBuffer' if name=='vkFreeCommandBuffers' else 'VkDescriptorSet'
        count='commandBufferCount' if name=='vkFreeCommandBuffers' else 'descriptorSetCount'
        field='pCommandBuffers' if name=='vkFreeCommandBuffers' else 'pDescriptorSets'
        condition='true' if name=='vkFreeCommandBuffers' else 'result==VK_SUCCESS'
        guest.append(f'    if({condition})for(uint32_t i=0;i<a.{count};i++)decoder.handles.erase(T_{kind},reinterpret_cast<uint64_t>(a.{field}[i]));\n')
    if name=='vkResetDescriptorPool':guest.append('    if(result==VK_SUCCESS)decoder.handles.erase_children(T_VkDescriptorPool,reinterpret_cast<uint64_t>(a.descriptorPool));\n')
    parent=None
    if name=='vkAllocateCommandBuffers':parent=('VkCommandPool','a.pAllocateInfo->commandPool')
    elif name=='vkAllocateDescriptorSets':parent=('VkDescriptorPool','a.pAllocateInfo->descriptorPool')
    elif first_type in ('VkInstance','VkPhysicalDevice','VkDevice'):parent=(first_type,'a.'+first)
    if parent:guest.append(f'    uint64_t parent=decoder.handles.token(T_{parent[0]},reinterpret_cast<uint64_t>({parent[1]}),decoder.good);\n')
    guest.append('    decoder.finish();\n')
    if parent:guest.append('    decoder.parent_outputs(parent);\n')
    # Free named objects after translating the response; no stale driver handles.
    if name.startswith('vkDestroy') or name=='vkFreeMemory':
        idx=0 if name in ('vkDestroyInstance','vkDestroyDevice') else 1
        typ=commands[name].findall('param')[idx].findtext('type')
        guest.append(f'    decoder.handles.erase(T_{base(typ)},retired);\n')
    guest.append(f'    return decoder.good ? {"result" if ret=="VkResult" else "VK_SUCCESS"} : VK_ERROR_TOO_MANY_OBJECTS;\n }}\n')
client.append('} // extern C\n')
client.append('namespace ame::vk {\nPFN_vkVoidFunction proc(const char* name) {\n')
for name in names:
    if name in ('vkGetInstanceProcAddr','vkGetDeviceProcAddr'):continue
    client.append(f'    if(same(name,"{name}"))return reinterpret_cast<PFN_vkVoidFunction>({name});\n')
client.append('    return nullptr;\n}\n}\n')
guest.append(' default:return VK_ERROR_FEATURE_NOT_PRESENT;\n }\n}\n')
(a.output/'vk-rpc-client.cc').write_text(''.join(client));(a.output/'vk-rpc-guest.inc').write_text(''.join(guest))
print(f'Generated {len(roots)} commands / {len(entries)} types from Vulkan 1.0')

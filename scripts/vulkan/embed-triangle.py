#!/usr/bin/env python3
"""Rebuild checked-in native Vulkan shaders and validate Vulkan 1.0 SPIR-V."""
from pathlib import Path
import struct,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=root/'linux-artifacts/panvk-renderer'
def embed(name,variants,comment):
    lines=[comment+'\n#include <stdint.h>\n']
    with tempfile.TemporaryDirectory() as work:
        for symbol,path,defs in variants:
            binary=Path(work)/(symbol+'.spv')
            subprocess.run(['glslangValidator','-V','--target-env','vulkan1.0',*defs,str(source/path),'-o',str(binary)],check=True)
            subprocess.run(['spirv-val','--target-env','vulkan1.0',str(binary)],check=True)
            data=binary.read_bytes();words=struct.unpack('<'+str(len(data)//4)+'I',data)
            lines.append('static const uint32_t '+symbol+'[] = {\n')
            for i in range(0,len(words),8):lines.append('    '+','.join(f'0x{v:08x}' for v in words[i:i+8])+',\n')
            lines.append('};\n')
    (root/'shared/include/midix/render'/name).write_text(''.join(lines))
embed('vulkan_triangle_spirv.h',[(f'triangle_{s}',f'triangle.{s}',[]) for s in ('vert','frag')],
      '/* SPIR-V from linux-artifacts/panvk-renderer/triangle.vert and triangle.frag.\n * Regenerate with scripts/vulkan/embed-triangle.py. */')
variants=[]
for v in ('float','float_ms','uint','uint_ms','float_array','float_ms_array','uint_array','uint_ms_array'):
    variants.append(('probe_'+v,'probe.comp',(['-DMULTISAMPLED'] if 'ms' in v else [])+(['-DINTEGER_SOURCE'] if v.startswith('uint') else [])+(['-DLAYERED_SOURCE'] if v.endswith('array') else [])))
for v in ('vertex','vertex_inputs','fragment','fragment_uint'):
    variants.append(('probe_'+v,'probe.vert' if v.startswith('vertex') else 'probe.frag',
                     ['-DVERTEX_INPUT'] if v=='vertex_inputs' else ['-DINTEGER_SOURCE'] if v=='fragment_uint' else []))
for v in ('storage_float','storage_unorm','storage_atomic'):
    variants.append(('probe_'+v,'storage.comp',['-DNORMALIZED_STORAGE'] if v=='storage_unorm' else ['-DATOMIC_STORAGE'] if v=='storage_atomic' else []))
embed('vulkan_probe_spirv.h',variants,'/* Vulkan 1.0 probe SPIR-V. Regenerate with scripts/vulkan/embed-triangle.py. */')

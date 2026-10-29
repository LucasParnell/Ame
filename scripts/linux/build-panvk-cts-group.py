#!/usr/bin/env python3
"""Link a bounded CTS group package from the verified complete AArch64 build.

Only root registration changes. Test implementations, expected results and the
complete CTS build remain untouched. Grouped runs are development evidence and
must not be represented as a full CTS/conformance run.
"""
from pathlib import Path
import argparse,hashlib,json,re,shlex,subprocess,shutil
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--groups',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--build',type=Path,default=ROOT/'out/vulkan-cts-midgard-build')
p.add_argument('--source',type=Path,default=ROOT/'out/vulkan-cts-midgard')
a=p.parse_args();a.build=a.build.resolve();a.source=a.source.resolve();a.output=a.output.resolve()
groups=set(a.groups.split(','));a.output.mkdir(parents=True,exist_ok=True)
provenance=json.loads((a.build/'provenance.json').read_text())
if provenance['revision']!='7aa3551652743f11e77cc28dd8c19c9f872ecf79' or provenance['target_cpu']!='DE_CPU_ARM_64':raise ValueError('unverified base CTS')
full=a.build/'external/vulkancts/modules/vulkan/deqp-vk'
if hashlib.sha256(full.read_bytes()).hexdigest()!=provenance['binary_sha256']:raise ValueError('base binary changed')
original=a.source/'external/vulkancts/modules/vulkan/vktTestPackage.cpp'
text=original.read_text();start=text.index('void TestPackage::init(void)');end=text.index('\nvoid ExperimentalTestPackage::init(void)',start)
body=text[start:end];registrations=re.findall(r'    addRootChild\("([a-z0-9_]+)"[^\n]+',body)
if not groups or not groups.issubset(registrations):raise ValueError('unknown CTS root group')
body=re.sub(r'    addRootChild\("([a-z0-9_]+)"[^\n]+',lambda m:m[0] if m[1] in groups else '',body)
text=text[:start]+body+text[end:]
text=re.sub(r'void ExperimentalTestPackage::init\(void\)\n\{.*?\n\}', 'void ExperimentalTestPackage::init(void)\n{}',text,count=1,flags=re.S)
source=a.output/'vktTestPackage.cpp';source.write_text(text)
commands=subprocess.check_output(['ninja','-C',str(a.build),'-t','commands','deqp-vk'],text=True).splitlines()
compile_line=next(c for c in commands if ' -c '+str(original) in c)
compile_args=shlex.split(compile_line);filtered=[];i=0
while i<len(compile_args):
    arg=compile_args[i]
    if arg in ('-MT','-MF'):i+=2;continue
    if arg=='-MD':i+=1;continue
    filtered.append(arg);i+=1
obj=a.output/'vktTestPackage.o'
filtered[filtered.index('-o')+1]=str(obj);filtered[filtered.index('-c')+1]=str(source)
filtered+=['-ffunction-sections','-fdata-sections','-I'+str(original.parent)]
subprocess.run(filtered,cwd=a.build,check=True)
link_line=next(c for c in commands if ' -o external/vulkancts/modules/vulkan/deqp-vk ' in c)
args=shlex.split(link_line)
if args[:2]==[':','&&']:args=args[2:]
if args[-2:]==['&&',':']:args=args[:-2]
if any(v in args for v in ('&&',';','|')):raise ValueError('unexpected link command')
args=[v for v in args if not v.startswith('-Wl,--dependency-file=')]
output=a.output/'deqp-vk';args[args.index('-o')+1]=str(output)
idx=args.index('external/vulkancts/modules/vulkan/libdeqp-vk-package.a');args.insert(idx,str(obj))
first_archive=next(i for i,v in enumerate(args) if v.endswith('.a'))
args.insert(first_archive,'-Wl,--start-group')
args+=['-Wl,--end-group','-Wl,--gc-sections']
subprocess.run(args,cwd=a.build,check=True)
subprocess.run(['aarch64-linux-gnu-strip','--strip-unneeded',str(output)],check=True)
resources=a.output/'resources/vulkan'
if resources.exists():shutil.rmtree(resources)
resources.mkdir(parents=True)
resource_source=a.source/'external/vulkancts/data/vulkan'
excluded=[]
if 'video' not in groups:excluded.append('video')
if 'graphicsfuzz' not in groups:excluded.append('amber/graphicsfuzz')
if 'glsl' not in groups:excluded.append('glsl')
if 'spirv_assembly' not in groups:excluded.append('amber/spirv_assembly')
resource_files={}
for file in sorted(resource_source.rglob('*')):
    relative=file.relative_to(resource_source)
    if not file.is_file() or any(relative==Path(e) or Path(e) in relative.parents for e in excluded):continue
    target=resources/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(file,target)
    resource_files[str(relative)]=hashlib.sha256(file.read_bytes()).hexdigest()
(a.output/'provenance.json').write_text(json.dumps({'base':provenance,'registered_roots':sorted(groups),
    'test_implementations_modified':False,'registration_source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
    'binary_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'binary_bytes':output.stat().st_size,
    'full_cts_run':False,'hardware_execution':False,'resource_sha256':resource_files,'excluded_unregistered_group_resources':excluded},indent=2)+'\n')
print('Grouped CTS binary:',output,output.stat().st_size,'bytes; roots',','.join(sorted(groups)))

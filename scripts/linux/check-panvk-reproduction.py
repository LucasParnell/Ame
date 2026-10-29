#!/usr/bin/env python3
"""Verify the recorded PanVK source from the pinned archive and patch series."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,tarfile,tempfile
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'out/mesa-panvk-midgard')
p.add_argument('--archive',type=Path,default=ROOT/'out/mesa-panfrost/mesa-24.3.4.tar.xz')
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();manifest=json.loads((a.build/'source-manifest.json').read_text())
archive_hash=hashlib.sha256(a.archive.read_bytes()).hexdigest()
if archive_hash!=manifest['mesa_archive_sha256']:raise ValueError('archive hash mismatch')
patches=[ROOT/'third_party/mesa/panvk-midgard'/name for name in manifest['patches']]
if [hashlib.sha256(p.read_bytes()).hexdigest() for p in patches]!=manifest['patch_sha256']:raise ValueError('patch hashes changed')
paths=set(manifest['sources'])
for patch in patches:
    for line in patch.read_text().splitlines():
        if line.startswith(('--- a/','+++ b/')):paths.add(line[6:].split('\t')[0])
for path in paths:
    if Path(path).is_absolute() or '..' in Path(path).parts:raise ValueError('invalid manifest path')
with tempfile.TemporaryDirectory(prefix='panvk-reproduce-') as temporary:
    stage=Path(temporary)
    # Stream the XZ archive once. Random extraction repeatedly decompresses
    # the same source archive for each file and wastes CPU on this host.
    prefix='mesa-'+manifest['mesa_version']+'/'
    with tarfile.open(a.archive,mode='r|xz') as archive:
        for member in archive:
            if not member.name.startswith(prefix):continue
            path=member.name[len(prefix):]
            if path not in paths:continue
            if not member.isfile():raise ValueError('unexpected archive entry')
            target=stage/path;target.parent.mkdir(parents=True,exist_ok=True)
            with archive.extractfile(member) as source,target.open('wb') as output:shutil.copyfileobj(source,output)
    for patch in patches:
        with patch.open('rb') as source:
            subprocess.run(['patch','--batch','--forward','--silent','-p1','-d',str(stage)],stdin=source,check=True)
    mismatches=[name for name,expected in manifest['sources'].items()
                if not (stage/name).is_file() or hashlib.sha256((stage/name).read_bytes()).hexdigest()!=expected]
report={'archive_sha256':archive_hash,'patches':manifest['patches'],'patch_count':len(patches),
        'verified_files':len(manifest['sources']),'mismatches':mismatches,'reproduced':not mismatches}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
print('PanVK reproduction:',len(patches),'patches,',len(manifest['sources']),'source files,',len(mismatches),'mismatches')
raise SystemExit(bool(mismatches))

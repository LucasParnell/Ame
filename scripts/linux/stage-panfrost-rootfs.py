#!/usr/bin/env python3
"""Stage only the AArch64 Mesa/Panfrost ELF dependency closure, with hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def stage(agent, mesa, sysroot, output, extra_libraries=(), extra_executables=(), icd_json=None, cts_data=None):
    output.mkdir(parents=True, exist_ok=True)
    root = output / 'rootfs'
    root.mkdir(exist_ok=True)
    search = [mesa / 'lib', sysroot / 'lib', sysroot / 'lib64']
    pending = [(agent, Path('/init'))]
    for library in extra_libraries:
        if not re.fullmatch(r'lib[A-Za-z0-9_.+-]+\.so(?:\.[0-9]+)*', library.name):
            raise ValueError('unsafe extra library name')
        pending.append((library, Path('/usr/lib') / library.name))
        search.insert(0, library.parent)
    for executable in extra_executables:
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.+-]*', executable.name):
            raise ValueError('unsafe executable name')
        pending.append((executable, Path('/usr/bin') / executable.name))
    files = {}
    while pending:
        source, target = pending.pop()
        if str(target) in files:
            continue
        data = source.read_bytes()
        if data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20], 'little') != 183:
            raise ValueError(f'not an AArch64 ELF: {source}')
        dynamic = subprocess.check_output(['aarch64-linux-gnu-readelf', '-dW', str(source)], text=True)
        for needed in re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic):
            if '/' in needed or '..' in needed:
                raise ValueError('unsafe dependency name')
            found = next((p / needed for p in search if (p / needed).is_file()), None)
            if not found:
                raise ValueError(f'unresolved dependency: {source}: {needed}')
            pending.append((found, Path('/usr/lib') / needed))
        interpreter = re.search(r'Requesting program interpreter: (.*?)]',
            subprocess.check_output(['aarch64-linux-gnu-readelf', '-lW', str(source)], text=True))
        if interpreter:
            target_loader = Path(interpreter[1])
            loader = next((p / target_loader.name for p in search if (p / target_loader.name).is_file()), None)
            if not loader:
                raise ValueError('ELF interpreter missing')
            pending.append((loader, target_loader))
        # /init is supplied separately to mkinitramfs.py.
        dest = root / target.relative_to('/') if target != Path('/init') else output / 'dango-agent'
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        dest.chmod(0o755)
        subprocess.run(['aarch64-linux-gnu-strip', '--strip-unneeded', str(dest)], check=True)
        files[str(target)] = {'source': str(source.resolve()), 'bytes': dest.stat().st_size,
            'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()}
    def stage_data(source, target):
        destination=root / target.relative_to('/')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o644)
        files[str(target)]={'source': str(source.resolve()), 'bytes': destination.stat().st_size,
            'sha256': hashlib.sha256(destination.read_bytes()).hexdigest()}
    if icd_json:
        manifest=json.loads(icd_json.read_text())
        if manifest.get('ICD', {}).get('library_path')!='/usr/lib/libvulkan_panfrost.so':
            raise ValueError('CTS manifest must select the staged PanVK ICD')
        stage_data(icd_json, Path('/usr/share/vulkan/icd.d/panfrost_icd.aarch64.json'))
    if cts_data:
        if not cts_data.is_dir() or cts_data.is_symlink():
            raise ValueError('invalid CTS resources')
        for source in sorted(cts_data.rglob('*')):
            if source.is_symlink(): raise ValueError('CTS resource symlink')
            if source.is_file():stage_data(source,Path('/usr/share/deqp/vulkan') / source.relative_to(cts_data))
    # Dependencies live in /usr/lib. The renderer uses RPATH; other executables
    # must set LD_LIBRARY_PATH (the cross loader uses multilib search paths).
    # The interpreter retains its ABI-mandated /lib pathname.
    (root / 'usr/lib/dri').mkdir(parents=True, exist_ok=True)
    (output / 'mesa-runtime.json').write_text(json.dumps({'files': files,
        'renderer': 'panfrost', 'cts_executables': [p.name for p in extra_executables], 'experimental_panvk': bool(extra_libraries), 'software_drivers': False}, indent=2) + '\n')
    print('Panfrost runtime:', len(files), 'ELFs,', sum(f['bytes'] for f in files.values()), 'bytes')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--agent', required=True, type=Path)
    p.add_argument('--mesa', required=True, type=Path)
    p.add_argument('--sysroot', type=Path, default=Path('/usr/aarch64-linux-gnu'))
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--extra-library', action='append', default=[], type=Path)
    p.add_argument('--extra-executable', action='append', default=[], type=Path)
    p.add_argument('--icd-json', type=Path)
    p.add_argument('--cts-data', type=Path)
    a = p.parse_args()
    stage(a.agent, a.mesa, a.sysroot, a.output, a.extra_library, a.extra_executable, a.icd_json, a.cts_data)

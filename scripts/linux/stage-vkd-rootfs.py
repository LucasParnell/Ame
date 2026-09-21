#!/usr/bin/env python3
"""Stage the native arm64 libc/lavapipe ELF closure, without a shell or package manager."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.check_output(args, text=True, env=dict(os.environ, LC_ALL='C'))


def stage(output):
    libraries = {}
    for line in run('ldconfig', '-p').splitlines():
        match = re.match(r'\s*(\S+) .* => (\S+)$', line)
        if match:
            libraries.setdefault(match[1], Path(match[2]))
    manifests = sorted(Path('/usr/share/vulkan/icd.d').glob('lvp_icd*.json'))
    if len(manifests) != 1:
        raise ValueError('expected exactly one packaged lavapipe ICD manifest')
    manifest = json.loads(manifests[0].read_text())
    icd = Path(manifest['ICD']['library_path'])
    if not icd.is_absolute():
        icd = libraries.get(str(icd), manifests[0].parent / icd)
    if not icd.is_file():
        raise ValueError(f'lavapipe library missing: {icd}')
    destination = output / 'rootfs'
    if destination.is_symlink():
        raise ValueError(f'refusing symlink output: {destination}')
    with tempfile.TemporaryDirectory(prefix='.vkd-rootfs-', dir=output) as work:
        root = Path(work)
        pending = [(output / 'dango-vkd', Path('/usr/bin/dango-vkd')),
                   (output / 'vkd-probe', Path('/usr/bin/vkd-probe')), (icd, icd)]
        copied = {}
        while pending:
            source, target = pending.pop()
            if str(target) in copied:
                continue
            if not target.is_absolute() or '..' in target.parts:
                raise ValueError(f'unsafe guest path: {target}')
            header = source.read_bytes()[:20]
            if header[:6] != b'\x7fELF\x02\x01' or int.from_bytes(header[18:20], 'little') != 183:
                raise ValueError(f'not a little-endian AArch64 ELF: {source}')
            dynamic = run('readelf', '-dW', str(source))
            for name in re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic):
                dependency = libraries.get(name)
                if dependency is None:
                    raise ValueError(f'unresolved ELF dependency: {source}: {name}')
                pending.append((dependency, dependency))
            interpreter = re.search(r'Requesting program interpreter: (.*?)]',
                                    run('readelf', '-lW', str(source)))
            if interpreter:
                loader = Path(interpreter[1])
                pending.append((loader, loader))
            dest = root / target.relative_to('/')
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
            dest.chmod(0o755)
            copied[str(target)] = hashlib.sha256(dest.read_bytes()).hexdigest()
        manifest['ICD']['library_path'] = str(icd)
        dest = root / 'usr/share/vulkan/icd.d/lvp_icd.json'
        dest.parent.mkdir(parents=True)
        dest.write_text(json.dumps(manifest, indent=2) + '\n')
        for directory in ('dev', 'proc', 'sys', 'run', 'tmp'):
            (root / directory).mkdir()
        (root / 'tmp').chmod(0o1777)
        if destination.exists():
            shutil.rmtree(destination)
        root.rename(destination)
    provenance = {'files': dict(sorted(copied.items())), 'packages': run(
        'dpkg-query', '-W', '-f=${Package} ${Version}\n', 'libc6', 'libvulkan1',
        'mesa-vulkan-drivers'), 'uncompressed_bytes': sum(
            p.stat().st_size for p in destination.rglob('*') if p.is_file())}
    (output / 'rootfs-manifest.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(f"Vulkan rootfs ELF closure: {len(copied)} files, {provenance['uncompressed_bytes']} bytes")


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(f'usage: {sys.argv[0]} OUTPUT_DIRECTORY')
    stage(Path(sys.argv[1]).resolve())

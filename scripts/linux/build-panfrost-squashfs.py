#!/usr/bin/env python3
"""Package an already staged renderer in an optional compressed read-only root."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def build(output):
    output = output.resolve()
    subprocess.run([sys.executable, str(ROOT / 'scripts/linux/check-minimal-config.py'),
                    '--config', str(output / 'linux.config'), '--flow', 'panfrost-squashfs'], check=True)
    if not shutil.which('mksquashfs'):
        raise ValueError('mksquashfs is required for the optional compressed runtime')
    stage = output / 'rootfs'
    agent = output / 'dango-agent'
    if not stage.is_dir() or not agent.is_file():
        raise ValueError('stage the renderer before packaging its compressed runtime')
    # Work in a new directory so the ordinary staged dependency tree stays usable.
    with tempfile.TemporaryDirectory(prefix='squashfs-', dir=output) as temporary:
        work = Path(temporary)
        runtime = work / 'runtime'
        shutil.copytree(stage, runtime)
        shutil.copyfile(agent, runtime / 'init')
        (runtime / 'init').chmod(0o755)
        for name in ('dev', 'proc', 'sys', 'tmp', 'run'):
            (runtime / name).mkdir(exist_ok=True)
        bootstrap = work / 'bootstrap'
        subprocess.run(['aarch64-linux-gnu-gcc', '-std=c11', '-O2', '-static', '-nostdlib',
                        '-ffreestanding', '-fno-stack-protector', '-no-pie', '-Wall', '-Wextra',
                        '-Werror', '-Wl,-e,_start', str(ROOT / 'linux-artifacts/panfrost-renderer/rootfs-init.c'),
                        '-o', str(bootstrap)], check=True)
        initroot = work / 'initroot'
        initroot.mkdir()
        (initroot / 'root').mkdir()
        image = initroot / 'runtime.sqfs'
        subprocess.run(['mksquashfs', str(runtime), str(image), '-noappend', '-comp', 'xz',
                        '-b', '131072', '-processors', '1', '-mem', '256M', '-all-root',
                        '-no-xattrs', '-mkfs-time', '0', '-all-time', '0'], check=True)
        spec = importlib.util.spec_from_file_location('ame_mkinitramfs', ROOT / 'scripts/linux/mkinitramfs.py')
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        archive = work / 'initramfs.cpio.gz'
        module.build(bootstrap, archive, initroot)
        subprocess.run([sys.executable, str(ROOT / 'scripts/linux/check-initramfs.py'), str(archive)], check=True)
        report = {
            'storage': 'squashfs-xz', 'guest_layout': 'minimal',
            'runtime_bytes': sum(p.stat().st_size for p in runtime.rglob('*') if p.is_file()),
            'squashfs_bytes': image.stat().st_size,
            'squashfs_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
            'bootstrap_sha256': hashlib.sha256(bootstrap.read_bytes()).hexdigest(),
            'initramfs_bytes': archive.stat().st_size,
            'initramfs_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        }
        archive.replace(output / 'initramfs.cpio')
        (output / 'rootfs-storage.json').write_text(json.dumps(report, indent=2) + '\n')
    if (output / 'dango-linux.dtb').is_file():
        subprocess.run([sys.executable, str(ROOT / 'scripts/linux/update-initrd-dtb.py'), str(output)], check=True)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args().output)

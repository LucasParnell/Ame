#!/usr/bin/env python3
"""Package a headless Ame QEMU evaluation for an isolated TWRP /tmp directory.

This does not build a native Huawei kernel or flashable phone image. Alpine's
signed packages are verified by apk on the phone before QEMU is launched.
"""

import argparse
import copy
import hashlib
import json
import posixpath
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / 'scripts/anne/evaluation-runtime.lock.json'
CDN = 'https://dl-cdn.alpinelinux.org/alpine'


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError('unsafe archive path: ' + name)
    return str(path)


def normalize_rootfs(source, destination):
    """Keep archive links inside its root and reject link-parent traversal."""
    with tarfile.open(source, 'r:gz') as archive:
        members = archive.getmembers()
        if len(members) > 20000 or sum(m.size for m in members) > 128 * 1024**2:
            raise ValueError('rootfs exceeds expected bounds')
        links = {safe_name(m.name) for m in members if m.issym() or m.islnk()}
        for member in members:
            name = safe_name(member.name)
            if any(str(parent) in links for parent in PurePosixPath(name).parents):
                raise ValueError('archive entry has a link parent: ' + name)
            if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
                raise ValueError('unsupported archive member: ' + name)
        with tarfile.open(destination, 'w:gz') as output:
            for original in members:
                member = copy.copy(original)
                member.name = safe_name(member.name)
                member.uid = member.gid = 0
                member.uname = member.gname = ''
                member.mode &= 0o777
                member.pax_headers = {}
                if member.issym():
                    parent = posixpath.dirname(member.name) or '.'
                    target = member.linkname
                    target = (target.lstrip('/') if target.startswith('/') else
                              posixpath.normpath(posixpath.join(parent, target)))
                    target = safe_name(target)
                    member.linkname = posixpath.relpath(target, parent)
                elif member.islnk():
                    member.linkname = safe_name(member.linkname)
                output.addfile(member, archive.extractfile(original) if member.isfile() else None)


SETUP = '''#!/sbin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "$base" in /tmp/ame-evaluation-*) ;; *) echo "Stage under /tmp/ame-evaluation-*" >&2; exit 1;; esac
cd "$base"
sha256sum -c SHA256SUMS
if [ -e rootfs ]; then echo "Runtime already extracted; use a fresh staging directory" >&2; exit 1; fi
mkdir rootfs
tar -xzf minirootfs.tar.gz -C rootfs
root="$base/rootfs"
"$root/lib/ld-musl-aarch64.so.1" --library-path "$root/lib:$root/usr/lib" \\
  "$root/sbin/apk" --root "$root" --keys-dir "$root/etc/apk/keys" \\
  --repositories-file /dev/null --no-network --no-scripts --force-non-repository \\
  add "$base"/packages/*.apk
"$root/lib/ld-musl-aarch64.so.1" --library-path "$root/lib:$root/usr/lib" \\
  "$root/usr/bin/qemu-system-aarch64" --version
touch runtime-ready
'''

LAUNCH = '''#!/sbin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case "$base" in /tmp/ame-evaluation-*) ;; *) echo "Stage under /tmp/ame-evaluation-*" >&2; exit 1;; esac
[ -f "$base/runtime-ready" ] || { echo "Run setup.sh first" >&2; exit 1; }
if [ -e "$base/qemu.pid" ]; then echo "Inspect the previous run before launching again" >&2; exit 1; fi
root="$base/rootfs"
exec "$root/lib/ld-musl-aarch64.so.1" --library-path "$root/lib:$root/usr/lib" \\
  "$root/usr/bin/qemu-system-aarch64" -L "$root/usr/share/qemu" \\
  -machine virt,virtualization=off,gic-version=2 -accel tcg,thread=single \\
  -cpu cortex-a53 -smp 1 -m 256M -display none -nic none -monitor none \\
  -serial "file:$base/ame.log" -kernel "$base/guest/anko.bin" \\
  -initrd "$base/guest/system.mxb" -daemonize -pidfile "$base/qemu.pid"
'''


def verified_file(path, expected, download_url=None):
    if not path.exists() and download_url:
        subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error',
                        '--retry', '2', '--max-time', '90', '-o', str(path), download_url], check=True)
    if not path.is_file() or digest(path) != expected:
        raise ValueError('missing file or SHA-256 mismatch: ' + str(path))


def build(cache, build_dir, output, download=False):
    lock = json.loads(LOCK.read_text())
    if lock['schema'] != 1 or lock['architecture'] != 'aarch64':
        raise ValueError('unsupported runtime lock')
    # Restrict lock fields before constructing filenames or official URLs.
    for package in lock['packages']:
        for key in ('name', 'version'):
            if not re.fullmatch(r'[A-Za-z0-9+_.-]+', package[key]):
                raise ValueError('invalid package ' + key)
        if package['repository'] not in ('main', 'community'):
            raise ValueError('invalid repository')
    if output.exists():
        raise FileExistsError('output exists: ' + str(output))
    cache.mkdir(parents=True, exist_ok=True)
    (cache / 'packages').mkdir(exist_ok=True)
    branch = lock['alpine_branch']
    if branch != 'v3.22':
        raise ValueError('unexpected Alpine branch')
    root_record = lock['minirootfs']
    root_name = safe_name(root_record['file'])
    if '/' in root_name:
        raise ValueError('invalid rootfs filename')
    source_root = cache / root_name
    root_url = f'{CDN}/{branch}/releases/aarch64/{root_name}'
    verified_file(source_root, root_record['sha256'], root_url if download else None)
    sources = []
    for package in lock['packages']:
        name = f"{package['name']}-{package['version']}.apk"
        source = cache / 'packages' / name
        url = f"{CDN}/{branch}/{package['repository']}/aarch64/{name}"
        verified_file(source, package['sha256'], url if download else None)
        if source.stat().st_size != package['size']:
            raise ValueError('package size mismatch: ' + name)
        sources.append((source, url))
    guests = {'anko.bin': build_dir / 'kernel/anko.bin',
              'system.mxb': build_dir / 'system.mxb',
              'provenance.json': build_dir / 'generated/provenance.json'}
    for path in guests.values():
        if not path.is_file():
            raise ValueError('build the evaluation guest and provenance first: ' + str(path))
    provenance = json.loads(guests['provenance.json'].read_text())
    manifest = json.loads((build_dir / 'manifest.json').read_text())
    if (provenance['build']['board'] != 'qemu-virt' or
            provenance['artifact']['sha256'] != digest(guests['system.mxb']) or
            {s['name'] for s in manifest['services']} != {'nameservice', 'io.console'}):
        raise ValueError('expected current qemu-virt build with the minimal anne service policy')
    output.mkdir(parents=True)
    (output / 'packages').mkdir()
    (output / 'guest').mkdir()
    normalize_rootfs(source_root, output / 'minirootfs.tar.gz')
    for source, _ in sources:
        shutil.copyfile(source, output / 'packages' / source.name)
    for name, source in guests.items():
        shutil.copyfile(source, output / 'guest' / name)
    for name, body in [('setup.sh', SETUP), ('launch.sh', LAUNCH)]:
        (output / name).write_text(body)
        (output / name).chmod(0o755)
    shutil.copyfile(LOCK, output / 'runtime.lock.json')
    manifest = {'schema': 1, 'execution': 'QEMU TCG inside TWRP; not native Huawei boot',
                'guest_board': 'qemu-virt', 'mochi_profile': 'huawei-anne',
                'rootfs_source': {'url': root_url, 'sha256': root_record['sha256']},
                'apk_urls': [url for _, url in sources],
                'signature_verification': 'apk with bundled Alpine trust keys, before launch',
                'package_sources': 'https://gitlab.alpinelinux.org/alpine/aports',
                'source_archive_base': 'https://distfiles.alpinelinux.org/distfiles/v3.22/'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    files = sorted(path for path in output.rglob('*') if path.is_file())
    (output / 'SHA256SUMS').write_text(''.join(f'{digest(path)}  {path.relative_to(output).as_posix()}\n'
                                            for path in files))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / 'out/anne-evaluation-build')
    parser.add_argument('--build', type=Path, default=ROOT / 'out/anne-qemu-dev')
    parser.add_argument('--output', type=Path, required=True, help='new package directory')
    parser.add_argument('--download', action='store_true', help='fetch missing pinned files from Alpine')
    args = parser.parse_args()
    build(args.cache, args.build, args.output, args.download)
    print('Created headless recovery evaluation: ' + str(args.output))


if __name__ == '__main__':
    main()

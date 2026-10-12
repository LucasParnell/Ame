#!/usr/bin/env python3
"""Build/check Switchroot BL31 against Ame's Hekate Erista firmware profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.request

from mx.mxld import parse_elf

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / 'boards/switch-t210'


def profile():
    return json.loads((BOARD / 'firmware.json').read_text())


def reserved_range():
    config = profile()
    # Standard Hekate SECFW profile: a 1 MiB firmware region immediately below
    # BL31, and a 1 MiB GPU firmware/WPR region below that. Both remain reserved
    # even when Ame doesn't run a GPU driver.
    base, size, prefix = (int(config[key], 0) for key in
                          ('tzdram_base', 'tzdram_size', 'reserved_prefix_size'))
    return base - prefix, size + prefix


def linked_base(data):
    """Decode the reviewed ATF reset handler, not an arbitrary address literal.

    ADR x18, entry; MOVZ x17, BL31_BASE; CMP x18, x17; B.EQ already_relocated.
    Require ADR to resolve to byte zero. Unknown/ambiguous firmware is rejected.
    This is a compatibility check, not firmware authentication.
    """
    matches = []
    for offset in range(0, len(data) - 15, 4):
        adr, mov, cmp, branch = struct.unpack_from('<4I', data, offset)
        if adr & 0x9f00001f != 0x10000012 or mov & 0xff80001f != 0xd2800011:
            continue
        if cmp != 0xeb11025f or branch & 0xff00001f != 0x54000000:
            continue
        displacement = ((adr >> 5) & 0x7ffff) << 2 | ((adr >> 29) & 3)
        if displacement & (1 << 20):
            displacement -= 1 << 21
        if offset + displacement != 0:
            continue
        matches.append(((mov >> 5) & 0xffff) << (((mov >> 21) & 3) * 16))
    if len(matches) != 1:
        raise ValueError('unknown or ambiguous ATF reset handler; use scripts/build-switch-atf.sh')
    return matches[0]


def check(path):
    config = profile()
    data = path.read_bytes()
    if not 16 <= len(data) <= int(config['tzdram_size'], 0):
        raise ValueError('BL31 size is outside the configured secure-memory aperture')
    actual = linked_base(data)
    expected = int(config['tzdram_base'], 0)
    if actual != expected:
        raise ValueError(f'BL31 is linked at {actual:#x}, but {config["profile"]} requires '
                         f'{expected:#x}; rebuild with scripts/build-switch-atf.sh')
    return hashlib.sha256(data).hexdigest()


def check_source_ranges(source, work, config):
    """Exercise the firmware's actual range checker and macro arithmetic."""
    platform = source / 'plat/nvidia/tegra'
    text = (platform / 'common/tegra_bl31_setup.c').read_text()
    start = text.index('int32_t bl31_check_ns_address(')
    end = text.index('\n}', start) + 2
    macros = []
    for header, names in (
        ('include/platform_def.h', ('TZDRAM_END',)),
        ('include/t210/tegra_def.h', ('TEGRA_DRAM_BASE', 'TEGRA_DRAM_END')),
    ):
        content = (platform / header).read_text()
        for name in names:
            match = re.search(r'^#define\s+' + name + r'\s+([^\n]+)$', content, re.M)
            if not match:
                raise ValueError(f'missing firmware definition: {name}')
            macros.append(f'#define {name} {match[1]}\n')
    (work / 'atf-range-function.inc').write_text(''.join(macros) + text[start:end] + '\n')
    probe = work / 'range-probe'
    subprocess.run(['clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                    '-I', str(work), '-DTZDRAM_BASE=' + config['tzdram_base'],
                    '-DTZDRAM_SIZE=' + config['tzdram_size'],
                    str(ROOT / 'tests/atf_range_probe.c'), '-o', str(probe)], check=True)
    subprocess.run([str(probe)], check=True)


def build(output, archive, jobs):
    config = profile()
    output.mkdir(parents=True, exist_ok=True)
    # Fresh extraction prevents cached objects from carrying a previous layout.
    with tempfile.TemporaryDirectory(prefix='atf-', dir=output) as tmp:
        work = Path(tmp)
        if archive is None:
            archive = work / 'source.tar.gz'
            with urllib.request.urlopen(config['source_url'], timeout=60) as response:
                with archive.open('wb') as target:
                    shutil.copyfileobj(response, target)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != config['source_sha256']:
            raise ValueError('ATF source archive checksum mismatch')
        with tarfile.open(archive) as source:
            source.extractall(work, filter='data')
        source_dir = work / ('switch-atf-' + config['source_commit'])
        patch = BOARD / 'atf-layout.patch'
        subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=source_dir, check=True)
        check_source_ranges(source_dir, work, config)
        subprocess.run(['make', f'-j{jobs}', 'PLAT=tegra', 'TARGET_SOC=t210',
                        'TZDRAM_BASE=' + config['tzdram_base'],
                        'TZDRAM_SIZE=' + config['tzdram_size'],
                        'CC=clang', 'LD=ld.lld', 'AR=llvm-ar', 'OC=llvm-objcopy',
                        'OD=llvm-objdump', 'BUILD_STRING=ame-' + config['source_commit'],
                        *[f'{key}={value}' for key, value in config['atf_options'].items()],
                        'bl31'], cwd=source_dir, check=True)
        artifacts = source_dir / 'build/tegra/t210/release'
        elf = artifacts / 'bl31/bl31.elf'
        entry, segments = parse_elf(elf)
        base, size = (int(config[key], 0) for key in ('tzdram_base', 'tzdram_size'))
        if entry != base or any(s['virtual_address'] < base or
                               s['virtual_address'] + s['memory_size'] > base + size
                               for s in segments):
            raise ValueError('BL31 ELF entry/segments exceed the configured secure aperture')
        digest = check(artifacts / 'bl31.bin')
        for source, name in ((elf, 'bl31.elf'), (artifacts / 'bl31.bin', 'bl31.bin')):
            shutil.copy2(source, output / name)
        metadata = dict(config, binary_sha256=digest,
                        patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                        compiler=subprocess.check_output(['clang', '--version'], text=True).splitlines()[0])
        (output / 'provenance.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Validated BL31: {output / "bl31.bin"} ({digest})')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    verify = actions.add_parser('check')
    verify.add_argument('image', type=Path)
    reservations = actions.add_parser('reservations')
    reservations.add_argument('output', type=Path)
    compile_ = actions.add_parser('build')
    compile_.add_argument('--output', type=Path, default=ROOT / 'build-switch/atf')
    compile_.add_argument('--archive', type=Path)
    compile_.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    try:
        if args.action == 'check':
            print(check(args.image))
        elif args.action == 'reservations':
            base, size = reserved_range()
            args.output.write_text(f'/memreserve/ 0x{base:016x} 0x{size:016x};\n')
        else:
            if args.jobs < 1:
                raise ValueError('--jobs must be positive')
            build(args.output.resolve(), args.archive.resolve() if args.archive else None, args.jobs)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'error: {error}\n')


if __name__ == '__main__':
    main()

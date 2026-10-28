#!/usr/bin/env python3
"""Build an exact-image, RAM-only BL31 inspection profile; never flash storage.

Callback 2 (DDR statistics copy) is replaced only in experimental RAM profiles.
The smmu-id variant also replaces one redundant mapping and the SP0 synchronous
vector with a narrowly gated recovery handler. Outputs are private; this program
never handles fuses, wrapping keys or shared-key captures.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BL31_SHA = '27c06575c5af056fdbedc9385d4bd3c460459f5075ee79dcb65762f71fa5eb85'
LK_SHA = 'b42e40323c270b649cb0f554e7959c78d09b577275b25af63b38c6693c531e17'
CALLBACK, CALLBACK_END = 0x58e8, 0x5a10
HOOK, PATCH = 0x2bc100, 0x10b720


def sha(data):
    return hashlib.sha256(data).hexdigest()


def assemble(source, output, address, symbols=''):
    linker = output.with_suffix('.ld')
    linker.write_text('OUTPUT_FORMAT("elf64-littleaarch64")\n'
        'OUTPUT_ARCH(aarch64)\n' + symbols + '\n'
        f'SECTIONS {{ . = {address}; .text : {{ *(.text) }} '
        '/DISCARD/ : { *(.comment .note* .eh_frame*) } }\n')
    elf = output.with_suffix('.elf')
    subprocess.run(['clang', '--target=aarch64-none-elf', '-nostdlib', '-fuse-ld=lld',
        '-Wl,--build-id=none', f'-Wl,-e,{address}', '-Wl,-T,' + str(linker),
        str(source), '-o', str(elf)], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', str(elf), str(output)], check=True)
    return output.read_bytes()


def build(bl31_path, lk_path, output, variant='policy'):
    if variant not in ('policy', 'smmu-id'):
        raise ValueError('Unknown bounded probe variant')
    bl31, lk = bl31_path.read_bytes(), lk_path.read_bytes()
    if sha(bl31) != BL31_SHA or sha(lk) != LK_SHA:
        raise ValueError('Unreviewed input firmware hash')
    if len(bl31) % 16 or struct.unpack_from('<I', lk, PATCH)[0] != 0x17fbd30a:
        raise ValueError('Unexpected monitor alignment or final handoff instruction')
    # Captured partition tails contain stale RAM, not necessarily zeros.
    # This is the same post-image trampoline slot used by the validated
    # anne-el2 kernel_preload profile, beyond the linked image extent.
    if struct.unpack_from('<Q', lk, 0x3e0)[0] != 0x2bbcb0:
        raise ValueError('Unexpected linked loader extent')
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    output.chmod(0o700)
    source = ROOT / 'scripts/anne'
    probe_source = 'monitor_smmu_probe.S' if variant == 'smmu-id' else 'monitor_probe.S'
    probe = assemble(source / probe_source, output / 'probe.bin', 0x35e00000 + CALLBACK)
    if len(probe) > CALLBACK_END - CALLBACK:
        raise ValueError('Probe exceeds the replaced callback')
    monitor = bytearray(bl31)
    monitor[CALLBACK:CALLBACK + len(probe)] = probe
    if variant == 'smmu-id':
        # Replace a redundant DMSS mapping, retaining its existing superset
        # at 0x13570. This adds no mmap records and changes no effective DMSS
        # permissions. The new candidate mapping is secure Device, read-only.
        duplicate = struct.pack('<QQQQ', 0xfffc0000, 0xfffc0000, 0x8000, 8)
        superset = struct.pack('<QQQQ', 0xfffc0000, 0xfffc0000, 0xd000, 8)
        if bl31[0x13630:0x13650] != duplicate or bl31[0x13570:0x13590] != superset:
            raise ValueError('Unexpected DMSS duplicate mapping')
        monitor[0x13630:0x13650] = struct.pack('<QQQQ', 0xe8dc0000, 0xe8dc0000, 0x1000, 0)
        symbols = {}
        nm = subprocess.check_output(['llvm-nm', str(output / 'probe.elf')], text=True)
        for line in nm.splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2] in ('smmu_probe_load', 'smmu_probe_return'):
                symbols[fields[2]] = int(fields[0], 16)
        guard = assemble(source / 'monitor_smmu_recover.S', output / 'recover.bin', 0x35e1a800,
            f'probe_load = {symbols["smmu_probe_load"]}; '
            f'probe_return = {symbols["smmu_probe_return"]}; stock_unhandled = 0x35e11574;')
        original_vector = struct.pack('<I', 0x94000000 | (((0x11574 - 0x1a800) // 4) & 0x3ffffff))
        if len(guard) > 0x80 or bl31[0x1a800:0x1a804] != original_vector or any(bl31[0x1a804:0x1a880]):
            raise ValueError('Unexpected current-EL SP0 vector or oversized recovery guard')
        monitor[0x1a800:0x1a800 + len(guard)] = guard
    preload = assemble(source / 'monitor_preload.S', output / 'preload.bin', HOOK,
                       f'monitor_bytes = {len(monitor)};')
    if len(preload) > 0x100:
        raise ValueError('Preload exceeds the reserved trampoline range')
    loader = bytearray(lk)
    loader[HOOK:HOOK + len(preload)] = preload
    struct.pack_into('<I', loader, PATCH, 0x14000000 | ((HOOK - PATCH) // 4))
    (output / 'monitor.bin').write_bytes(monitor)
    (output / 'loader.bin').write_bytes(loader)
    (output / 'original-monitor.bin').write_bytes(bl31)
    manifest = dict(version=3, monitor_sha256=sha(monitor), loader_sha256=sha(loader),
        monitor_source_sha256=BL31_SHA, loader_source_sha256=LK_SHA,
        callback_offset=CALLBACK, callback_bytes=len(probe), preload_offset=HOOK,
        preload_bytes=len(preload), handoff_offset=PATCH,
        handoff_instruction=f'{struct.unpack_from("<I", loader, PATCH)[0]:08x}',
        monitor_bytes=len(monitor), monitor_staging=0x03000000,
        monitor_upload_address=0x13000000, monitor_destination=0x35e00000,
        smc=0xc500aa01, callback_cookie=0x55bbcce2, argument_cookie=0x414d4552,
        signature_selector=1, signature=0x414d0003, persistent_writes=False,
        statistics_read_selector=6, statistics_set_selector_base=0x100,
        statistics_writable_mask=0x7f,
        protection_read_selector_base=0x80, protection_read_words=64,
        protection_read_base=0xfffc3500,
        replaced_service='DDR statistics copy callback 2; experiment only')
    if variant == 'smmu-id':
        for key in ('statistics_read_selector', 'statistics_set_selector_base',
                    'statistics_writable_mask', 'protection_read_selector_base',
                    'protection_read_words', 'protection_read_base'):
            del manifest[key]
        manifest.update(version=4, variant=variant, signature=0x414d0004,
            read_only=True, candidate_base=0xe8dc0000, id_selectors=[2, 3, 4],
            recovery_check_selector=5, recovery_check_address=0x20000000,
            vector_offset=0x1a800, vector_bytes=len(guard),
            candidate_mapping_offset=0x13630)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for path in output.iterdir():
        if path.is_file():
            path.chmod(0o600)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bl31', type=Path, required=True)
    parser.add_argument('--lk', type=Path, default=ROOT / 'out/anne-device-private/el2-firmware/stock-lk-el2-tee.bin')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--variant', choices=('policy', 'smmu-id'), default='policy')
    args = parser.parse_args()
    print(json.dumps(build(args.bl31, args.lk, args.output, args.variant), indent=2))


if __name__ == '__main__':
    main()

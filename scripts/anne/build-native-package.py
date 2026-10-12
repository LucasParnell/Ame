#!/usr/bin/env python3
"""Build an experimental native Huawei kernel container and verified restore image.

Uses the captured stock Android v0 kernel header, preserving its load addresses
and firmware fields. No phone commands, flashing, or boot are performed here.
"""
import argparse
import base64
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
AVB_COMMIT = '761178607206f4cb2af79ed9eec52d8cbd814adb'
AVB_SHA256 = 'e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b'
AVB_URL = f'https://android.googlesource.com/platform/external/avb/+/{AVB_COMMIT}/avbtool.py?format=TEXT'
DEFAULT_AVBTOOL = ROOT / 'out/host-tools/avb/avbtool.py'
DEFAULT_AVBKEY = ROOT / 'out/anne-native-build/dev-avb-key.pem'
SPEC = importlib.util.spec_from_file_location('board_image', ROOT / 'scripts/check-board-image.py')
board_image = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(board_image)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def stock_header(data):
    if len(data) != 24 * 1024**2 or data[:8] != b'ANDROID!':
        raise ValueError('expected the backed-up 24 MiB Huawei kernel partition')
    words = struct.unpack_from('<10I', data, 8)
    size, address, ramdisk_size, _, second_size, _, tags, page, version, _ = words
    if (address != 0x00480000 or tags != 0x07e00000 or page != 2048 or
            ramdisk_size or second_size or version or size < 64 or size > len(data) - page):
        raise ValueError('unexpected stock kernel layout; re-inspect the actual firmware')
    payload = data[page:page + size]
    if payload[:2] != b'\x1f\x8b':
        raise ValueError('expected gzip stock kernel')
    inflater = zlib.decompressobj(16 + zlib.MAX_WBITS)
    raw = inflater.decompress(payload, 128 * 1024**2 + 1)
    if not inflater.eof or inflater.unused_data or len(raw) > 128 * 1024**2:
        raise ValueError('stock kernel gzip stream exceeds bounds or has trailing data')
    if raw[56:60] != b'ARM\x64' or struct.unpack_from('<Q', raw, 8)[0] != 0x80000:
        raise ValueError('stock ARM64 header does not confirm the native image offset')
    expected_id = hashlib.sha1(payload + struct.pack('<III', size, 0, 0)).digest()
    if data[576:596] != expected_id:
        raise ValueError('stock legacy kernel hash does not match its payload')
    return data[:page]


def kernel_container(template, image):
    header = bytearray(stock_header(template))
    if len(image) < 64 or image[56:60] != b'ARM\x64':
        raise ValueError('expected a native ARM64 Image')
    offset, footprint, flags = struct.unpack_from('<3Q', image, 8)
    # Keep the first image below Huawei's observed reserved training/tag memory.
    if (offset != 0x80000 or footprint < len(image) or footprint > 64 * 1024**2 or flags & 1):
        raise ValueError('unsupported native Image placement/size/endianness')
    compressed = gzip.compress(image, mtime=0)
    struct.pack_into('<I', header, 8, len(compressed))
    header[576:608] = hashlib.sha1(compressed + struct.pack('<III', len(compressed), 0, 0)).digest() + bytes(12)
    padding = bytes((-len(compressed)) % len(header))
    result = bytes(header) + compressed + padding
    if len(result) > len(template):
        raise ValueError('native image does not fit kernel partition')
    return result


def footer_layout(stock):
    # This firmware's usable AVB partition ends one page before the block
    # device ends. A conventional footer at the final 64 bytes is incompatible.
    effective_size = len(stock) - 4096
    footer = stock[effective_size - 64:effective_size]
    if len(footer) != 64:
        raise ValueError('missing stock AVB footer')
    magic, major, minor, original, offset, size, reserved = struct.unpack('>4sIIQQQ28s', footer)
    if (magic != b'AVBf' or major != 1 or minor or reserved != bytes(28) or
            original < 2048 or original > offset or offset > effective_size - 64 or
            size < 256 or size > effective_size - 64 - offset):
        raise ValueError('unexpected Huawei stock AVB footer placement/bounds')
    header = stock[offset:offset + 256]
    if header[:4] != b'AVB0' or struct.unpack_from('>I', header, 28)[0] != 1:
        raise ValueError('expected stock SHA256_RSA2048 AVB metadata')
    rollback = struct.unpack_from('>Q', header, 112)[0]
    flags, location = struct.unpack_from('>II', header, 120)
    if flags or location:
        raise ValueError('unexpected stock AVB flags/rollback location')
    return effective_size, rollback


def verified_avbtool(path, download=False):
    if not path.exists() and download:
        path.parent.mkdir(parents=True, exist_ok=True)
        response = subprocess.run(['curl', '--fail', '--location', '--silent', '--show-error',
                                   '--retry', '2', '--max-time', '90', AVB_URL],
                                  capture_output=True, check=True)
        source = base64.b64decode(response.stdout, validate=False)
        if sha(source) != AVB_SHA256:
            raise ValueError('official avbtool download hash mismatch')
        path.write_bytes(source)
    if not path.is_file() or sha(path.read_bytes()) != AVB_SHA256:
        raise ValueError('missing or modified pinned avbtool; use --download-avbtool')


def add_avb(stock, container, avbtool, key):
    verified_avbtool(avbtool)
    if not key.is_file():
        raise ValueError('provide a local development RSA-2048 PEM with --avb-key')
    effective, rollback = footer_layout(stock)
    with tempfile.TemporaryDirectory(prefix='ame-anne-avb-') as directory:
        # avbtool verifies hash descriptors using their partition name.
        image = Path(directory) / 'kernel.img'
        image.write_bytes(container)
        subprocess.run([sys.executable, str(avbtool), 'add_hash_footer', '--image', str(image),
                        '--partition_name', 'kernel', '--partition_size', str(effective),
                        '--algorithm', 'SHA256_RSA2048', '--key', str(key),
                        '--rollback_index', str(rollback), '--salt', '00' * 32],
                       capture_output=True, check=True)
        verified = subprocess.run([sys.executable, str(avbtool), 'verify_image',
                                   '--image', str(image), '--key', str(key)],
                                  capture_output=True, text=True, check=True)
        metadata = subprocess.run([sys.executable, str(avbtool), 'info_image', '--image', str(image)],
                                  capture_output=True, text=True, check=True)
        physical = image.read_bytes() + stock[effective:]
    if len(physical) != len(stock):
        raise ValueError('AVB-packaged image does not match the physical kernel partition size')
    return physical, verified.stdout, metadata.stdout


def build(build_dir, backups, output, probe=False, avbtool=DEFAULT_AVBTOOL, key=DEFAULT_AVBKEY,
          minimal=False, stock_control=False):
    if output.exists():
        raise FileExistsError('output exists: ' + str(output))
    if sum([probe, minimal, stock_control]) > 1:
        raise ValueError('choose one diagnostic image')
    board = json.loads((build_dir / 'board.json').read_text())
    if board['board'] != 'huawei-anne' or int(board['load_base'], 0) != 0x00480000:
        raise ValueError('unexpected native board configuration')
    if not minimal and not stock_control and board_image.check(build_dir, 'huawei-anne') != 0x00480000:
        raise ValueError('unexpected native ELF entry')
    backup_manifest = json.loads((backups / 'manifest.json').read_text())
    stock = (backups / 'kernel.img').read_bytes()
    if (backup_manifest['kernel']['bytes'] != len(stock) or
            backup_manifest['kernel']['sha256'] != sha(stock)):
        raise ValueError('stock kernel backup is corrupt')
    target = 'anko-minimal' if minimal else ('anne-entry-probe' if probe else 'anko')
    if stock_control:
        stock_header(stock)
        size = struct.unpack_from('<I', stock, 8)[0]
        raw = gzip.decompress(stock[2048:2048 + size])
        bundle = b''
        provenance = {'build': {'board': 'huawei-anne', 'target': 'stock-packaging-control'},
                      'artifact': {'sha256': sha(raw), 'stock_partition_sha256': sha(stock)}}
    else:
        raw = (build_dir / 'kernel' / (target + '.bin')).read_bytes()
    if minimal:
        bundle = b''
        provenance = {'build': {'board': 'huawei-anne', 'target': target},
                      'artifact': {'sha256': sha(raw)},
                      'sources': {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in
                                  sorted((ROOT / 'boards/huawei-anne/minimal').iterdir()) if p.is_file()}}
        diagnostic_path = ROOT / 'boards/huawei-anne/minimal/diagnostic.json'
        diagnostic_info = json.loads(diagnostic_path.read_text()) if diagnostic_path.exists() else None
    elif not stock_control:
        provenance = json.loads((build_dir / 'generated/provenance.json').read_text())
        bundle = (build_dir / 'system.mxb').read_bytes()
        if provenance['build']['board'] != 'huawei-anne' or provenance['artifact']['sha256'] != sha(bundle):
            raise ValueError('native build provenance is stale or belongs to another board')
    if probe or minimal:
        elf = (build_dir / 'kernel' / target).read_bytes()
        if (elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 183 or
                struct.unpack_from('<Q', elf, 24)[0] != 0x00480000):
            raise ValueError('unexpected native diagnostic ELF entry/architecture')
        if minimal:
            # The Image footprint must cover every loadable segment, including
            # BSS and both private stacks, before the firmware places ramdisks.
            phoff = struct.unpack_from('<Q', elf, 32)[0]
            phsize, phnum = struct.unpack_from('<HH', elf, 54)
            end = 0x00480000
            for i in range(phnum):
                kind, _, _, va, pa, file_size, memory_size, _ = struct.unpack_from(
                    '<II6Q', elf, phoff + i * phsize)
                if kind != 1:
                    continue
                if va != pa or pa < 0x00480000 or file_size > memory_size:
                    raise ValueError('unsupported minimal ELF load segment')
                file_offset = struct.unpack_from('<Q', elf, phoff + i * phsize + 8)[0]
                if raw[pa - 0x00480000:pa - 0x00480000 + file_size] != elf[file_offset:file_offset + file_size]:
                    raise ValueError('minimal raw Image differs from its ELF')
                end = max(end, pa + memory_size)
            if end - 0x00480000 != struct.unpack_from('<Q', raw, 16)[0]:
                raise ValueError('minimal Image does not cover ELF memory footprint')
    elif not stock_control and bundle not in raw:
        raise ValueError('native image does not contain the current signed MXB')
    if not stock_control:
        elf = (build_dir / 'kernel' / target).read_bytes()
        provenance['kernel'] = {'target': target, 'image_sha256': sha(raw), 'elf_sha256': sha(elf)}
    image, avb_validation, avb_metadata = add_avb(stock, kernel_container(stock, raw), avbtool, key)
    output.mkdir(parents=True, mode=0o700)
    (output / 'ame-native-kernel.img').write_bytes(image)
    (output / 'restore-stock-kernel.img').write_bytes(stock)
    if not stock_control:
        # Keep the exact executable that produced this Image for fault-PC decoding.
        (output / 'kernel.elf').write_bytes(elf)
        (output / 'kernel.bin').write_bytes(raw)
    (output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    (output / 'avb-validation.txt').write_text(avb_validation)
    (output / 'avb-metadata.txt').write_text(avb_metadata)
    manifest = {'schema': 1, 'board': 'huawei-anne', 'model': 'ANE-LX1',
                'firmware': 'ANE-LX1 8.0.0.151(C782)', 'status': 'experimental; native phone boot untested',
                'purpose': ('unchanged stock kernel through the native packaging pipeline' if stock_control else
                            'minimal Anko native diagnostics' if minimal else
                            ('visible native entry probe' if probe else 'native Anko and Mochi')),
                'avb': {'algorithm': 'SHA256_RSA2048', 'signer': 'local development key; not Huawei',
                        'avbtool_commit': AVB_COMMIT, 'avbtool_sha256': AVB_SHA256,
                        'effective_partition_bytes': len(stock) - 4096,
                        'trailing_firmware_bytes': 4096, 'local_signature_and_hash_verified': True,
                        'bootloader_acceptance': 'unverified; requires the already unlocked bootloader'},
                'boot_route': 'unlocked Huawei bootloader -> gzip ARM64 ' + ('stock Image' if stock_control else 'Anko Image'),
                'partition': 'kernel', 'partition_bytes': len(stock), 'load_base': '0x00480000',
                'text_offset': '0x00080000', 'native_image_bytes': len(image),
                'native_image_sha256': sha(image), 'restore_kernel_sha256': sha(stock),
                'raw_kernel_sha256': sha(raw), 'signed_bundle_sha256': None if probe or minimal or stock_control else sha(bundle),
                'log_collection': ('recovery ADB if the unchanged stock kernel boots' if stock_control else
                                   'AMEFL record through Huawei DFX; LED stages; framebuffer when supported' if minimal else
                                   'live green EL1 / magenta EL2 screen; no persistent log' if probe else
                                   'AMEANKO1 checkpoints, console and fault registers in five ramoops zones; read from TWRP after stock restore'),
                'effect': 'temporarily replaces the shared kernel; Android/TWRP require stock kernel restoration'}
    if minimal and diagnostic_info:
        manifest['diagnostic'] = diagnostic_info
    elif not (probe or stock_control):
        cache = (build_dir / 'CMakeCache.txt').read_text()
        capture_line = next((line for line in cache.splitlines()
                             if line.startswith('ANKO_ANNE_DIAG_CAPTURE_SECONDS:')), '')
        capture_seconds = int(capture_line.split('=', 1)[1]) if capture_line else 0
        def cache_value(name, default=''):
            return next((line.split('=', 1)[1] for line in cache.splitlines()
                         if line.startswith(name + ':')), default)
        manifest['diagnostic'] = {'tag': 'AMEANKO1', 'transport': 'five retained ramoops records',
                                  'kernel_elf_sha256': sha(elf), 'counter_hz': 1920000,
                                  'fault_fields': ['last', 'esr', 'pc', 'far', 'sp'],
                                  'reset': 'LPM3 NMI bit 2; three-second fault LED hold',
                                  'capture_seconds': capture_seconds,
                                  'watchdog_retention_after_observation': cache_value('ANKO_ANNE_DIAG_WATCHDOG_RETENTION', 'OFF').upper() in ('ON', 'TRUE', 'YES', '1'),
                                  'reset_at_services_ready': cache_value('ANKO_ANNE_DIAG_RESET_AT_READY', 'ON').upper() in ('ON', 'TRUE', 'YES', '1'),
                                  'retention': 'transport verified with v6; this full-kernel image not yet phone tested'}
        run_tag = cache_value('ANKO_ANNE_DIAG_RUN_TAG', 'untagged')
        if run_tag != 'untagged': manifest['diagnostic']['run_tag'] = run_tag
        console_enabled = cache_value('ANKO_ANNE_FRAMEBUFFER', 'OFF').upper() in ('ON', 'TRUE', 'YES', '1')
        manifest['diagnostic']['framebuffer_console'] = console_enabled
        manifest['diagnostic']['watchdog_keepalive'] = {
            'enabled': cache_value('ANKO_ANNE_WATCHDOG', 'OFF').upper() in ('ON', 'TRUE', 'YES', '1'),
            'device': 'inherited SP805 at 0xe8a06000',
            'policy': 'reload existing timeout on CPU0; preserve control and clocks; refuse expired or unsupported state'}
        if console_enabled:
            manifest['diagnostic']['console'] = {
                'inset': {'left': 64, 'top': 224, 'bottom': 96},
                'banner': {'left': 64, 'top': 144, 'scale': 3},
                'font': '8x16 at 2x scale; 59 columns and 61 rows',
                'early_replay': True,
                'service_output': 'existing io.console log drain via native debug syscall',
                'cache_policy': 'normal memory; clean dirty framebuffer rows for display DMA',
                'extra_ready_hold_seconds': int(cache_value('ANKO_ANNE_DIAG_READY_HOLD_SECONDS', '0'))}
        display_value = next((line.split('=', 1)[1] for line in cache.splitlines()
                              if line.startswith('ANKO_ANNE_DISPLAY_CAPTURE:BOOL=')), 'OFF')
        display_enabled = display_value.upper() in ('ON', 'TRUE', 'YES', '1')
        manifest['diagnostic']['display_capture'] = display_enabled
        if display_enabled:
            manifest['diagnostic']['display'] = {
                'snapshots': ['entry (MMU off)', 'mmu.done (Device MMIO mappings)'],
                'paint': 'inherited physical linear fullscreen RGB only; reject unverified paths',
                'hold_seconds': 5,
                'reserved_pool': '0x5d000000+0x03000000',
                'setup': 'no display-controller programming; capture rejected path before implementing setup'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(p.read_bytes())}  {p.name}\n'
                                            for p in sorted(output.iterdir()) if p.is_file()))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'out/anne-native-dev')
    parser.add_argument('--backups', type=Path, default=ROOT / 'out/anne-device-private/boot-backups')
    parser.add_argument('--output', required=True, type=Path)
    diagnostic = parser.add_mutually_exclusive_group()
    diagnostic.add_argument('--probe', action='store_true', help='package the bounded visible-entry diagnostic')
    diagnostic.add_argument('--minimal', action='store_true', help='package minimal Anko framebuffer output')
    diagnostic.add_argument('--stock-control', action='store_true',
                            help='repackage the unchanged stock kernel to isolate firmware/container acceptance')
    parser.add_argument('--avbtool', type=Path, default=DEFAULT_AVBTOOL)
    parser.add_argument('--avb-key', type=Path, default=DEFAULT_AVBKEY)
    parser.add_argument('--download-avbtool', action='store_true', help='fetch the pinned official AVB tool')
    args = parser.parse_args()
    verified_avbtool(args.avbtool, args.download_avbtool)
    print(json.dumps(build(args.build, args.backups, args.output, args.probe, args.avbtool.resolve(),
                          args.avb_key.resolve(), minimal=args.minimal,
                          stock_control=args.stock_control), indent=2))


if __name__ == '__main__':
    main()

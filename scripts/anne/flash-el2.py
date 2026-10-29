#!/usr/bin/env python3
"""Flash Ame through the validated ANE BootROM -> RAM stock LK/TEE EL2 route.

Firmware is private and ignored under out/anne-device-private/el2-firmware.
Adapted from p20re payloads/anne-el2/run.py and install_kernel.py. Only the
kernel partition is written; the EL2 bootloader and TEE preload stay in RAM.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import time

from p20re_transport import Factory, upload

ROOT = Path(__file__).resolve().parents[2]
XLOADER_SHA = '07c2ec41717b532b5f8d27968111539901854afacb0c0f35ac8b85dc08be0e81'
LK_SHA = 'b42e40323c270b649cb0f554e7959c78d09b577275b25af63b38c6693c531e17'
TEE_SHA = '00699c5b984be77db180dc274b567f981d9c17b8465d7efc351456f9107ae239'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inputs(package, firmware):
    manifest = json.loads((package / 'manifest.json').read_text())
    image = (package / 'ame-native-kernel.img').read_bytes()
    if (manifest['board'] != 'huawei-anne' or manifest['partition'] != 'kernel' or
            manifest['model'] != 'ANE-LX1' or len(image) != 0x1800000 or
            manifest['partition_bytes'] != len(image) or
            sha(image) != manifest['native_image_sha256'] or image[:8] != b'ANDROID!'):
        raise RuntimeError('Kernel package identity, size or hash mismatch')
    m = json.loads((firmware / 'manifest.json').read_text())
    blobs = {}
    for name, expected in [('xloader.img', XLOADER_SHA),
                           ('stock-lk-el2-tee.bin', LK_SHA), ('teeos-decrypted.bin', TEE_SHA)]:
        blobs[name] = (firmware / name).read_bytes()
        if sha(blobs[name]) != expected:
            raise RuntimeError('Firmware hash mismatch: ' + name)
    # Pin the addresses as well as the binary; a changed manifest must not
    # redirect a verified payload into different RAM or alter the checks.
    expected = dict(sha256=LK_SHA, tee_sha256=TEE_SHA, patch_offset=0x10b634,
                    patched_instruction='52807921', tee_staging=0x01000000,
                    tee_upload_address=0x11000000, tee_patch_offset=0x10bd3c,
                    tee_patch_instruction='1406c0b1', diagnostic_dispatch_patch=0x21550)
    if any(m.get(k) != v for k, v in expected.items()):
        raise RuntimeError('Unexpected EL2 firmware manifest')
    return image, m, blobs


def wait_bootrom_tty(comports, timeout=15):
    # USB enumeration precedes both the tty node and udev's access rules.
    # Wait for that read-only discovery to settle before starting any upload.
    deadline = time.monotonic() + timeout
    while True:
        ports = [p for p in comports() if (p.vid, p.pid) == (0x12d1, 0x3609)]
        if len(ports) > 1:
            raise RuntimeError('Multiple Huawei BootROM ttys; refusing ambiguous upload')
        if ports and os.access(ports[0].device, os.R_OK | os.W_OK):
            return ports[0].device
        if time.monotonic() >= deadline:
            raise RuntimeError('Need exactly one accessible Huawei BootROM tty')
        time.sleep(.1)


def enter_lk(blobs, m):
    import serial
    import serial.tools.list_ports
    import usb.core
    tty = wait_bootrom_tty(serial.tools.list_ports.comports)
    with serial.Serial(tty, baudrate=115200, timeout=2, write_timeout=10,
                       dsrdtr=True, rtscts=True, exclusive=True) as port:
        port.reset_input_buffer()
        upload(port, 'signed xloader', blobs['xloader.img'], 0x20000)
        upload(port, 'RAM authentication patch', bytes.fromhex('00207047'), 0x211ac)
        upload(port, 'TEE preload', blobs['teeos-decrypted.bin'], m['tee_upload_address'])
        upload(port, 'stock LK with EL2 handoff', blobs['stock-lk-el2-tee.bin'], 0x10000000)
    print('Waiting for RAM LK USB...', flush=True)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if usb.core.find(idVendor=0x18d1, idProduct=0xd00d) is not None:
            return
        time.sleep(.5)
    raise RuntimeError('RAM LK did not enumerate')


def verify_lk(fb, m):
    for address, instruction in [(m['patch_offset'], m['patched_instruction']),
                                 (m['tee_patch_offset'], m['tee_patch_instruction']),
                                 (m['diagnostic_dispatch_patch'], 'd503201f')]:
        if fb.read('memory', address, 4) != struct.pack('<I', int(instruction, 16)):
            raise RuntimeError(f'Running LK patch mismatch at {address:#x}')
    tee = fb.read('memory', m['tee_staging'], m['tee_bytes'])
    if sha(tee) != TEE_SHA:
        raise RuntimeError('TEE staging RAM hash mismatch')
    if fb.command('getvar:nve:$FBLOCK').strip() != b'hex:00':
        raise RuntimeError('Expected already unlocked FBLOCK; no unlock write sent')
    print('Verified EL2/TEE RAM patches, full TEE staging hash and FBLOCK=0.', flush=True)


def install(fb, image, output, report, save, backup=False):
    value = fb.command('getvar:emmc:kernel').strip()
    match = re.fullmatch(rb'([0-9a-fA-F]{16}):([0-9a-fA-F]{16})', value)
    if not match:
        raise RuntimeError(f'Unexpected kernel geometry: {value!r}')
    start, size = [int(x, 16) for x in match.groups()]
    if size != len(image):
        raise RuntimeError('Kernel partition size differs from package')
    report.update(partition_start=start, partition_bytes=size)
    save()

    def capture(name):
        path = output / name
        with path.open('xb') as target:
            for offset in range(0, size, 0x20000):
                data = fb.read('emmc', start + offset, min(0x20000, size - offset))
                if len(data) != min(0x20000, size - offset):
                    raise RuntimeError('Short partition capture')
                target.write(data)
                if (offset + len(data)) % 0x200000 == 0:
                    print(f'{name}: {(offset + len(data)) // 1048576}/24 MiB', flush=True)
            target.flush()
            os.fsync(target.fileno())
        return sha(path.read_bytes())

    if backup:
        report['before_sha256'] = capture('kernel-before.img')
        save()
    command = f'download:{len(image):08x}'.encode().ljust(64, b'\0')
    time.sleep(.2)
    if fb.wr.write(command, timeout=5000) != len(command):
        raise RuntimeError('Short download command')
    response = bytes(fb.rd.read(64, timeout=15000)).rstrip(b'\0')
    if response != f'DATA{len(image):08x}'.encode():
        raise RuntimeError(f'Unexpected download response: {response!r}')
    for offset in range(0, len(image), 0x8000):
        chunk = image[offset:offset + 0x8000]
        if fb.wr.write(chunk, timeout=15000) != len(chunk):
            raise RuntimeError('Short kernel download')
    if fb.status().strip():
        raise RuntimeError('Unexpected download completion')
    report['write_started'] = True
    save()
    report['flash_response'] = fb.command('flash:kernel').decode(errors='replace')
    save()
    report['after_sha256'] = capture('kernel-after.img')
    report['readback_verified'] = report['after_sha256'] == sha(image)
    save()
    if not report['readback_verified']:
        raise RuntimeError('Kernel readback mismatch; boot not requested')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=Path)
    parser.add_argument('--firmware', type=Path,
                        default=ROOT / 'out/anne-device-private/el2-firmware')
    parser.add_argument('--serial', help='Expected handset fastboot serial; required for USB operations')
    parser.add_argument('--check', action='store_true', help='Verify local inputs without USB access')
    parser.add_argument('--running', action='store_true', help='Use verified EL2/TEE LK already in RAM')
    parser.add_argument('--no-boot', action='store_true', help='Leave RAM LK active after readback')
    parser.add_argument('--backup', action='store_true', help='Also capture the previous kernel before flashing')
    parser.add_argument('--enter-only', action='store_true', help='Enter and verify diagnostic RAM LK without flashing or booting')
    args = parser.parse_args()
    image, m, blobs = inputs(args.package, args.firmware)
    print('Verified package and all firmware hashes. Kernel SHA256:', sha(image), flush=True)
    if args.check:
        return
    if not args.serial:
        parser.error('--serial is required for USB operations')
    os.umask(0o077)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = ROOT / 'out/anne-device-private' / ('kernel-install-' + stamp)
    output.mkdir(mode=0o700)
    report = dict(package=str(args.package.resolve()), image_sha256=sha(image),
                  partition='kernel', write_started=False, readback_verified=False,
                  boot_command_sent=False, hardware_el2_verified=False, backup_requested=args.backup)

    def save():
        with (output / 'result.json').open('w') as target:
            json.dump(report, target, indent=2)
            target.write('\n')
            target.flush()
            os.fsync(target.fileno())

    save()
    print('Installation record:', output, flush=True)
    try:
        if not args.running:
            enter_lk(blobs, m)
        with Factory(args.serial) as fb:
            verify_lk(fb, m)
            report['ram_lk_verified'] = True
            save()
            if args.enter_only:
                print('Diagnostic EL2/TEE LK ready; no kernel write or boot requested.', flush=True)
                return
            install(fb, image, output, report, save, backup=args.backup)
            if not args.no_boot:
                # OEM boot preserves the RAM EL2 handoff; reboot would lose it.
                report['boot_response'] = fb.command('oem boot').decode(errors='replace')
                report['boot_command_sent'] = True
                save()
        print('Kernel partition flashed and full readback verified.', flush=True)
    except Exception as error:
        report['error'] = str(error)
        save()
        raise


if __name__ == '__main__':
    main()

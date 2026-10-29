#!/usr/bin/env python3
"""Boot the installed kernel through an experimental RAM-only monitor profile.

Explicit --boot is required. No eMMC write command exists in this runner.
Requires BootROM and a profile produced by build-monitor-probe.py. The normal
flash-el2.py input checks still authenticate the source kernel/LK/TEE assets.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import time

from p20re_transport import Factory, upload

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('anne_flash', Path(__file__).with_name('flash-el2.py'))
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, default=ROOT / 'out/anne-device-private/el2-firmware')
    parser.add_argument('--serial', default='89U4C18808003578')
    parser.add_argument('--boot', action='store_true')
    args = parser.parse_args()
    _, stock, blobs = flash.inputs(args.package, args.firmware)
    profile = json.loads((args.profile / 'manifest.json').read_text())
    loader = (args.profile / 'loader.bin').read_bytes()
    monitor = (args.profile / 'monitor.bin').read_bytes()
    # Rebuild from exact pinned inputs. A modified manifest alone cannot
    # authorize arbitrary RAM addresses, instructions, or monitor contents.
    builder_spec = importlib.util.spec_from_file_location('monitor_builder',
        Path(__file__).with_name('build-monitor-probe.py'))
    builder = importlib.util.module_from_spec(builder_spec)
    builder_spec.loader.exec_module(builder)
    import tempfile
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        original = args.profile / 'original-monitor.bin'
        if not original.exists():
            raise RuntimeError('Copy the pinned original plaintext to profile/original-monitor.bin')
        expected = builder.build(original, args.firmware / 'stock-lk-el2-tee.bin', work,
                                 profile.get('variant', 'policy'))
        if (profile != expected or loader != (work / 'loader.bin').read_bytes() or
                monitor != (work / 'monitor.bin').read_bytes()):
            raise RuntimeError('Profile differs from the reviewed deterministic build')
    print('Verified bounded RAM-only profile; installed kernel will be booted unchanged.', flush=True)
    if not args.boot:
        return
    import serial
    import serial.tools.list_ports
    import usb.core
    ports = [p for p in serial.tools.list_ports.comports() if (p.vid, p.pid) == (0x12d1, 0x3609)]
    if len(ports) != 1 or not os.access(ports[0].device, os.R_OK | os.W_OK):
        raise RuntimeError('Need exactly one accessible Huawei BootROM tty')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    record = args.profile / ('boot-' + stamp + '.json')
    report = dict(profile=profile, persistent_writes=False, boot_sent=False)
    def save():
        record.write_text(json.dumps(report, indent=2) + '\n')
        record.chmod(0o600)
    save()
    try:
        with serial.Serial(ports[0].device, baudrate=115200, timeout=2, write_timeout=10,
                           dsrdtr=True, rtscts=True, exclusive=True) as port:
            port.reset_input_buffer()
            upload(port, 'signed xloader', blobs['xloader.img'], 0x20000)
            upload(port, 'RAM authentication patch', bytes.fromhex('00207047'), 0x211ac)
            upload(port, 'TEE preload', blobs['teeos-decrypted.bin'], stock['tee_upload_address'])
            upload(port, 'experimental BL31 staging', monitor, profile['monitor_upload_address'])
            upload(port, 'experimental RAM LK', loader, 0x10000000)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if usb.core.find(idVendor=0x18d1, idProduct=0xd00d) is not None:
                break
            time.sleep(.25)
        else:
            raise RuntimeError('Experimental RAM LK did not enumerate')
        with Factory(args.serial) as fb:
            flash.verify_lk(fb, stock)
            if fb.read('memory', profile['handoff_offset'], 4) != struct.pack('<I', int(profile['handoff_instruction'], 16)):
                raise RuntimeError('RAM LK monitor hook mismatch')
            if fb.read('memory', profile['preload_offset'], profile['preload_bytes']) != (args.profile / 'preload.bin').read_bytes():
                raise RuntimeError('RAM LK preload mismatch')
            staged = fb.read('memory', profile['monitor_staging'], profile['monitor_bytes'])
            if hashlib.sha256(staged).hexdigest() != profile['monitor_sha256']:
                raise RuntimeError('Monitor staging hash mismatch')
            report['ram_verified'] = True
            save()
            report['boot_response'] = fb.command('oem boot').decode(errors='replace')
            report['boot_sent'] = True
            save()
        print('Experimental monitor boot sent; signature must be verified in Ame.', flush=True)
    except Exception as error:
        report['error'] = str(error)
        save()
        raise


if __name__ == '__main__':
    main()

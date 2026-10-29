#!/usr/bin/env python3
"""Flash a verified PanVK package and run the Vulkan + GLES hardware gate.

Waits for the handset to appear in Huawei BootROM download mode or as a verified
RAM LK, flashes the pinned image with a kernel backup and full readback
verification, then drives `diag gpu vulkan` followed by `diag gpu` over the Ame
CDC console. Writes a JSON report and exits 0 only when Vulkan executed GPU
work, restored the display, and GLES still rendered afterwards.

The wait has no fixed deadline by default: entering BootROM needs a physical
button sequence, and a 300-second window is what ended the previous attempt.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FLASH = ROOT / 'scripts/anne/flash-el2.py'
PACKAGE = ROOT / 'out/anne-panvk-experimental-v2'
FIRMWARE = ROOT / 'out/anne-device-private/el2-firmware'
APPROVED = '87c7d9be3c05a303771b7d513429e17a18ed4f717e5fed96df25bd5010815f7e'

BOOTROM = (0x12d1, 0x3609)
RAM_LK = (0x18d1, 0xd00d)

VULKAN = re.compile(
    rb'GPU: Vulkan status=(-?\d+) frames=(\d+) checksum=([0-9a-fA-F]+) display_restored=(\d+)')
GLES = re.compile(rb'GPU: Mali-T830 frames=(\d+)[^\r\n]*display restored')
FAULTS = ('GPU quarantined', 'Unhandled fault', 'Unhandled Page fault',
          'JOB_BUS_FAULT', 'TRANSLATION_FAULT', 'JOB_INVALID', 'DATA_INVALID',
          'MMU fault', 'GPU fault', 'USERFAULT', 'signal=11', 'signal=6')


def load_flash():
    spec = importlib.util.spec_from_file_location('flash_el2', FLASH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find(kind):
    """Return every USB device matching a (vid, pid) pair, or an empty list."""
    import usb.core
    return list(usb.core.find(find_all=True, idVendor=kind[0], idProduct=kind[1]) or [])


def wait_for_loader(flash, blobs, manifest, wait_seconds, poll=1.0):
    """Block until the handset offers a bootloader, uploading through BootROM.

    wait_seconds of 0 waits indefinitely. Returns the fastboot serial.
    """
    started = time.monotonic()
    announced = started
    print('Waiting for Huawei BootROM (12d1:3609) or verified RAM LK (18d1:d00d).', flush=True)
    print('Put the ANE-LX1 into BootROM download mode; nothing is written until it appears.',
          flush=True)
    while True:
        lk = find(RAM_LK)
        rom = find(BOOTROM)
        if len(lk) > 1 or len(rom) > 1:
            raise RuntimeError('More than one matching handset; refusing ambiguous selection')
        if lk:
            serial = lk[0].serial_number
            if not serial:
                raise RuntimeError('Missing bootloader device serial')
            print(f'RAM LK detected after {time.monotonic() - started:.0f}s.', flush=True)
            return serial
        if rom:
            print('BootROM detected; uploading the pinned RAM bootloader and TEE.', flush=True)
            flash.enter_lk(blobs, manifest)
            lk = find(RAM_LK)
            if len(lk) != 1:
                raise RuntimeError('Expected one RAM bootloader device after upload')
            serial = lk[0].serial_number
            if not serial:
                raise RuntimeError('Missing bootloader device serial')
            return serial
        if wait_seconds and time.monotonic() - started > wait_seconds:
            raise TimeoutError('Handset never entered BootROM; nothing was flashed')
        if time.monotonic() - announced > 30:
            announced = time.monotonic()
            print(f'Still waiting for BootROM ({announced - started:.0f}s elapsed).', flush=True)
        time.sleep(poll)


def open_console(timeout=180):
    """Wait for the Ame CDC console and take exclusive ownership of it."""
    sys.path.insert(0, str(ROOT / 'scripts/anne'))
    from serial_console import Serial
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ports = [p for p in Path('/dev/serial/by-id').glob('usb-Ame_*') if p.exists()]
        if len(ports) == 1:
            return Serial(str(ports[0]))
        time.sleep(0.5)
    raise RuntimeError('Ame USB serial did not enumerate after boot')


def reboot_to_bootrom():
    """Reboot the connected Ame handset; ANE's damaged xloader enters BootROM.

    This is opt-in. It writes no firmware and is used only after package/hash
    verification. The normal watcher still verifies the RAM loader afterward.
    """
    ports = [p for p in Path('/dev/serial/by-id').glob('usb-Ame_*') if p.exists()]
    if not ports:
        print('No Ame console; waiting for the existing bootloader state.', flush=True)
        return
    if len(ports) != 1:
        raise RuntimeError('Multiple Ame consoles; refusing ambiguous reboot')
    sys.path.insert(0, str(ROOT / 'scripts/anne'))
    from serial_console import Serial
    link = Serial(str(ports[0]))
    try:
        link.write(b'\x15reboot\r', time.monotonic() + 3)
        print('Sent reboot to Ame; waiting for BootROM enumeration.', flush=True)
        end = time.monotonic() + 5
        while time.monotonic() < end:
            try:
                link.read(min(end, time.monotonic() + 1))
            except TimeoutError:
                continue
            except OSError:
                break
    finally:
        link.close()


class Session:
    """Minimal console driver: send one command, capture its output."""

    def __init__(self, link, log):
        self.link, self.log = link, log

    def _emit(self, data):
        self.log.write(data)
        self.log.flush()

    def run(self, text, seconds, marker=None):
        self._emit(f'\n=== {text} ===\n'.encode())
        self.link.write(text.encode() + b'\r', time.monotonic() + 3)
        end = time.monotonic() + seconds
        data = bytearray()
        while time.monotonic() < end:
            try:
                part = self.link.read(min(end, time.monotonic() + 1))
            except TimeoutError:
                continue
            data.extend(part)
            self._emit(part)
            if marker and marker.search(data):
                break
        return data.decode(errors='replace')


def run_gate(output, timeout, require_load=False):
    """Drive the Vulkan and GLES diagnostics on a booted handset."""
    output.mkdir(parents=True, exist_ok=True)
    report = {'phone_gpu_execution': False, 'phone_display_restoration': False,
              'gles_after_vulkan': False}
    link = None
    with (output / 'hardware-test-console.log').open('wb') as log:
        try:
            link = open_console(timeout)
            shell = Session(link, log)
            shell.run('diag el2', 6)
            report['before_log'] = shell.run('diag log', 6)
            vulkan = shell.run('diag gpu vulkan', 60, VULKAN)
            report['vulkan_log'] = vulkan
            match = VULKAN.search(vulkan.encode())
            if match:
                status, frames, checksum, restored = match.groups()
                report['vulkan_status'] = int(status)
                report['frames'] = int(frames)
                report['checksum'] = checksum.decode()
                report['phone_display_restoration'] = int(restored) == 1
                report['phone_gpu_execution'] = (int(status) == 0 and int(frames) > 0
                                                 and int(checksum, 16) != 0)
            report['after_log'] = shell.run('diag log', 7)
            combined = report['after_log'] + vulkan
            report['fault_markers'] = [m for m in FAULTS if m in combined]
            report['phone_load_preservation'] = ('panvk: T830 LOAD preservation verified' in combined)
            # Always run GLES, even after a Vulkan fault. Skipping it hid whether
            # the working baseline survived, which is the regression that matters
            # most while the Vulkan path is still being brought up.
            gles = shell.run('diag gpu', 35, GLES)
            report['gles_log'] = gles
            gles_match = GLES.search(gles.encode())
            report['gles_frames'] = int(gles_match.group(1)) if gles_match else None
            report['gles_after_vulkan'] = bool(gles_match and int(gles_match.group(1)) > 0)
            report['after_gles_log'] = shell.run('diag log', 7)
            combined += gles + report['after_gles_log']
            report['fault_markers'] = [m for m in FAULTS if m in combined]
            if not gles_match and not report.get('error'):
                report['gles_probe'] = 'no GLES completion line after Vulkan'
            report['passed'] = bool(report['phone_gpu_execution']
                                    and report['phone_display_restoration']
                                    and report['gles_after_vulkan']
                                    and not report['fault_markers']
                                    and (not require_load or (report['phone_load_preservation']
                                         and report.get('frames', 0) > 1)))
        except Exception as error:  # noqa: BLE001 - the report is the product here
            report['error'] = str(error)
            report['passed'] = False
        finally:
            if link is not None:
                link.close()
    (output / 'phone-test.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='verify package and firmware hashes without USB access')
    parser.add_argument('--package', type=Path, default=PACKAGE,
                        help='package directory to flash (default: the approved v2 image)')
    parser.add_argument('--approve', default=APPROVED,
                        help='expected image SHA-256; pin a new build explicitly')
    parser.add_argument('--wait-seconds', type=int, default=0,
                        help='give up waiting for BootROM after this long; 0 waits forever')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'out/mesa-panvk-midgard/phone-v2')
    parser.add_argument('--reboot', action='store_true',
                        help='send reboot to Ame before waiting for BootROM (opt-in)')
    parser.add_argument('--test-only', action='store_true',
                        help='skip flashing and test the already-booted handset')
    parser.add_argument('--require-load', action='store_true',
                        help='also require the LOAD pixel probe and multiple Vulkan animation frames')
    parser.add_argument('--console-timeout', type=int, default=180)
    args = parser.parse_args()

    flash = load_flash()
    image, manifest, blobs = flash.inputs(args.package, FIRMWARE)
    digest = hashlib.sha256(image).hexdigest()
    if digest != args.approve:
        print(f'Package {args.package} hashes to {digest}, which is not the approved '
              f'{args.approve}.', flush=True)
        print(f'Pass --approve {digest} to pin this build.', flush=True)
        return 2
    print(f'Verified package and firmware hashes. Kernel SHA256: {digest}', flush=True)
    if args.check:
        return 0

    if not args.test_only:
        if args.reboot:
            reboot_to_bootrom()
        try:
            serial = wait_for_loader(flash, blobs, manifest, args.wait_seconds)
        except TimeoutError as error:
            print(str(error), flush=True)
            return 2
        print('RAM LK verified. Flashing with kernel backup and full readback check.', flush=True)
        subprocess.run([sys.executable, str(FLASH), '--package', str(args.package),
                        '--running', '--backup', '--serial', serial], check=True)
        print('Flash and readback verified. Waiting for the Ame console.', flush=True)

    report = run_gate(args.output, args.console_timeout, args.require_load)
    summary = {k: v for k, v in report.items() if not k.endswith('_log')}
    print('\nHardware gate result: ' + json.dumps(summary, indent=2), flush=True)
    print(f'Report: {args.output / "phone-test.json"}', flush=True)
    return 0 if report.get('passed') else 1


if __name__ == '__main__':
    sys.exit(main())

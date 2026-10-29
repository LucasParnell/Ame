"""Internal BootROM discovery and GPU diagnostic helpers for scripts/huawei."""
from __future__ import annotations

import importlib.util
import json
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

BOOTROM = (0x12d1, 0x3609)
RAM_LK = (0x18d1, 0xd00d)

VULKAN = re.compile(
    rb'GPU: Vulkan status=(-?\d+) frames=(\d+) checksum=([0-9a-fA-F]+) display_restored=(\d+)')
GLES = re.compile(rb'GPU: Mali-T830 frames=(\d+)[^\r\n]*display restored')
FAULTS = ('GPU quarantined', 'Unhandled fault', 'Unhandled Page fault',
          'JOB_BUS_FAULT', 'TRANSLATION_FAULT', 'JOB_INVALID', 'DATA_INVALID',
          'MMU fault', 'GPU fault', 'USERFAULT', 'signal=11', 'signal=6')


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
        if len(lk) + len(rom) > 1:
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
    """Wait for exactly one Ame CDC port on Linux or macOS."""
    spec = importlib.util.spec_from_file_location('anne_usb_sender',
                                                  Path(__file__).with_name('usb-input.py'))
    sender = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(Path(__file__).parent))
    spec.loader.exec_module(sender)
    from serial_console import Serial
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ports = sender.ame_serial_candidates()
        if len(ports) > 1:
            raise RuntimeError('Multiple Ame consoles; select the intended serial port')
        if ports:
            return Serial(ports[0])
        time.sleep(0.5)
    raise RuntimeError('Ame USB serial did not enumerate after boot')


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

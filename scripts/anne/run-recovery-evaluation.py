#!/usr/bin/env python3
"""Stage, test, and stop the headless Ame evaluation in TWRP's /tmp RAM storage."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


def verify_package(package):
    if any(p.is_symlink() for p in package.rglob('*')):
        raise ValueError('symlinks are not allowed in the staging package')
    lines = (package / 'SHA256SUMS').read_text().splitlines()
    files = set()
    for line in lines:
        expected, name = line.split('  ', 1)
        relative = Path(name)
        if (relative.is_absolute() or '..' in relative.parts or
                not (package / relative).resolve().is_relative_to(package.resolve())):
            raise ValueError('unsafe package path')
        if name in files or (package / relative).is_symlink():
            raise ValueError('duplicate path or symlink in package')
        files.add(name)
        with (package / relative).open('rb') as source:
            actual = hashlib.file_digest(source, 'sha256').hexdigest()
        if actual != expected:
            raise ValueError('package checksum mismatch: ' + name)
    required = {'setup.sh', 'launch.sh', 'minirootfs.tar.gz', 'manifest.json',
                'guest/anko.bin', 'guest/system.mxb', 'guest/provenance.json', 'runtime.lock.json'}
    present = {p.relative_to(package).as_posix() for p in package.rglob('*') if p.is_file()}
    if not required.issubset(files) or present != files | {'SHA256SUMS'}:
        raise ValueError('incomplete package or unlisted files')
    manifest = json.loads((package / 'manifest.json').read_text())
    if manifest['guest_board'] != 'qemu-virt' or manifest['mochi_profile'] != 'huawei-anne':
        raise ValueError('unexpected guest configuration')


class Device:
    def __init__(self, adb, serial=None, run=subprocess.run):
        self.adb, self.run = str(adb), run
        result = run([self.adb, 'devices'], capture_output=True, text=True, timeout=10, check=True)
        devices = []
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[1] in ('device', 'recovery'):
                devices.append(fields)
        if serial:
            devices = [d for d in devices if d[0] == serial]
        if len(devices) != 1:
            raise ValueError('select exactly one connected ADB device/recovery with --serial')
        self.serial = devices[0][0]
        codename = self.shell(['getprop', 'ro.product.device']).stdout.strip()
        self.twrp = self.shell(['getprop', 'ro.twrp.version']).stdout.strip()
        identity = self.shell(['id']).stdout
        if codename != 'anne' or not self.twrp or 'uid=0(' not in identity:
            raise ValueError('requires an anne phone running TWRP with a root ADB shell')

    def call(self, arguments, timeout=10, check=True):
        result = self.run([self.adb, '-s', self.serial, *arguments], capture_output=True,
                          text=True, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError((result.stdout + result.stderr).replace(self.serial, '<device>'))
        return result

    def shell(self, arguments, timeout=10, check=True):
        return self.call(['shell', shlex.join(arguments)], timeout, check)

    def process_matches(self, pid, prefix):
        if not pid.isdecimal():
            return False
        result = self.shell(['cat', '/proc/' + pid + '/cmdline'], check=False)
        return prefix + '/rootfs/usr/bin/qemu-system-aarch64' in result.stdout.split('\0')


def evaluate(package, output, adb, serial=None, timeout=45, run=subprocess.run):
    if output.exists():
        raise FileExistsError('output exists: ' + str(output))
    verify_package(package)
    device = Device(adb, serial, run)
    output.mkdir(parents=True, mode=0o700)
    prefix = '/tmp/ame-evaluation-' + uuid.uuid4().hex[:12]
    if device.shell(['test', '-e', prefix], check=False).returncode != 1:
        raise ValueError('temporary staging path is not available')
    result = {'schema': 1, 'execution': 'qemu-tcg-in-twrp', 'guest_board': 'qemu-virt',
              'services': ['nameservice', 'io.console'], 'remote_directory': prefix,
              'twrp_before_test': device.twrp, 'services_ready': False, 'guest_entry_el': None,
              'stop_exit_code': None, 'emulator_stopped': None}
    log, pid = '', ''
    start = time.monotonic()
    try:
        device.call(['push', str(package), prefix], timeout=90)
        setup = device.shell(['/sbin/sh', prefix + '/setup.sh'], timeout=90, check=False)
        (output / 'setup.log').write_text((setup.stdout + setup.stderr).replace(device.serial, '<device>'))
        if setup.returncode:
            raise RuntimeError('evaluation setup failed; see setup.log')
        launch = device.shell(['/sbin/sh', prefix + '/launch.sh'], timeout=20, check=False)
        (output / 'launch.log').write_text((launch.stdout + launch.stderr).replace(device.serial, '<device>'))
        if launch.returncode:
            raise RuntimeError('emulator launch failed; see launch.log')
        pid = device.shell(['cat', prefix + '/qemu.pid']).stdout.strip()
        if not pid.isdecimal():
            raise ValueError('invalid emulator PID')
        boot_start = time.monotonic()
        while time.monotonic() - boot_start < timeout:
            log = device.shell(['cat', prefix + '/ame.log'], check=False).stdout
            if 'boot_el=1' in log and 'init: all services ready' in log:
                result['services_ready'] = True
                result['guest_entry_el'] = 1
                break
            if not device.process_matches(pid, prefix):
                break
            time.sleep(1)
        if not result['services_ready']:
            raise RuntimeError('Ame did not reach EL1 service readiness; see ame.log')
    except Exception as error:
        result['error'] = str(error).replace(device.serial, '<device>')
        raise
    finally:
        # Never signal an unverified PID, TWRP, or unrelated host/phone process.
        try:
            if not pid:
                pid = device.shell(['cat', prefix + '/qemu.pid'], check=False).stdout.strip()
            if pid.isdecimal():
                if device.process_matches(pid, prefix):
                    result['stop_exit_code'] = device.shell(['kill', '-TERM', pid], check=False).returncode
                    for _ in range(10):
                        if not device.process_matches(pid, prefix):
                            break
                        time.sleep(0.2)
                result['emulator_stopped'] = not device.process_matches(pid, prefix)
            result['twrp_after_test'] = device.shell(['getprop', 'ro.twrp.version'], check=False).stdout.strip()
        except Exception as error:
            result['cleanup_error'] = str(error).replace(device.serial, '<device>')
            result['emulator_stopped'] = False
        result['elapsed_seconds'] = round(time.monotonic() - start, 3)
        (output / 'ame.log').write_text(log.replace(device.serial, '<device>'))
        result['log_sha256'] = hashlib.sha256((output / 'ame.log').read_bytes()).hexdigest()
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if not result['emulator_stopped'] or result.get('twrp_after_test') != device.twrp:
        raise RuntimeError('boot passed, but emulator cleanup/recovery access was not verified; see result.json')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new private evidence directory')
    parser.add_argument('--adb', default=shutil.which('adb') or str(ROOT / 'out/host-tools/platform-tools/adb'))
    parser.add_argument('--serial')
    parser.add_argument('--timeout', type=int, default=45)
    args = parser.parse_args()
    if not 5 <= args.timeout <= 120:
        parser.error('timeout must be between 5 and 120 seconds')
    result = evaluate(args.package.resolve(), args.output, args.adb, args.serial, args.timeout)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

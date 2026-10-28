#!/usr/bin/env python3
"""Build the full Ame system and flash it to a BootROM-mode ANE-LX1.

No arguments required. Default action:
  build out/anne-usb-native-release (anko, mochi_system, provenance)
  package a fresh full native kernel image
  flash it: BootROM -> RAM stock LK/TEE EL2 handoff -> kernel write -> boot

The handset fastboot serial is discovered from the RAM LK device itself, so it
never has to be typed. This reuses the verified build-native-package.py and
flash-el2.py helpers instead of reimplementing the BootROM transport or the AVB
container format.

    .venv/bin/python3 scripts/anne/flash-latest.py              # build, package, flash
    .venv/bin/python3 scripts/anne/flash-latest.py --dry-run    # build, package, verify only
    .venv/bin/python3 scripts/anne/flash-latest.py --reuse-latest
    .venv/bin/python3 scripts/anne/flash-latest.py --skip-build --backup

If pyserial/pyusb are missing it re-executes itself under .venv/bin/python3,
which is the failure ("No module named 'serial'") that aborts a system-python run.
"""
import argparse
import datetime
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = ROOT / 'out/anne-usb-native-release'
DEFAULT_KEY = Path.home() / '.ame-keys/anne-avb-key.pem'
FIRMWARE = ROOT / 'out/anne-device-private/el2-firmware'
BOOTROM = (0x12d1, 0x3609)    # Huawei BootROM download mode
FASTBOOT = (0x18d1, 0xd00d)   # RAM LK factory fastboot


def venv_python():
    return ROOT / '.venv/bin/python3'


def ensure_deps():
    """Re-exec under the project venv when pyserial/pyusb are not importable."""
    try:
        import serial  # noqa: F401
        import usb.core  # noqa: F401
        return
    except ImportError as error:
        candidate = venv_python()
        # .venv/bin/python3 is a symlink to the system interpreter, so compare
        # sys.prefix (the venv root) rather than the resolved executable path.
        in_venv = Path(sys.prefix).resolve() == (ROOT / '.venv').resolve()
        if candidate.exists() and not in_venv:
            print(f'missing {error.name}; re-executing under {candidate}', flush=True)
            os.execv(str(candidate), [str(candidate), *sys.argv])
        sys.exit(f'missing python module {error.name}; install pyserial and pyusb')


def load_flash_module():
    path = ROOT / 'scripts/anne/flash-el2.py'
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location('flash_el2', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command, *, env=None):
    print('+', ' '.join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, cwd=ROOT, env=env)


def refresh_renderer(build):
    """Prebuilt Linux artifacts still need their renderer rebuilt with native APIs."""
    cache = build / 'CMakeCache.txt'
    if not cache.is_file():
        return
    values = {}
    for line in cache.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line:
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    directory = values.get('DANGO_LINUX_ARTIFACT_DIR')
    if not directory or values.get('DANGO_BUILD_LINUX_ARTIFACTS') in ('ON', 'TRUE', '1'):
        return
    artifacts = Path(directory)
    if not artifacts.is_absolute():
        artifacts = ROOT / artifacts
    manifest_path = artifacts / 'manifest.toml'
    manifest = tomllib.loads(manifest_path.read_text())
    if manifest.get('profile') != 'panfrost' or manifest.get('renderer') != 'gles':
        return
    env = dict(os.environ, DANGO_LINUX_OUT=str(artifacts), DANGO_LINUX_PROFILE='panfrost',
               DANGO_LINUX_GUEST=manifest.get('guest', 'minimal'),
               DANGO_MANIFEST_NAME=manifest['name'],
               DANGO_PANFROST_RENDERER='gles',
               DANGO_PANFROST_HARDWARE=manifest.get('mali_hardware', '0'),
               DANGO_LINUX_BOOTARGS=manifest['bootargs'],
               DANGO_LINUX_KERNEL_LOAD=manifest['kernel_load'])
    run(['scripts/linux/build-panfrost-renderer.sh'], env=env)
    run(['scripts/linux/build-manifest.sh'], env=env)
    # Refresh the content hash used to embed the appliance into system.mxb.
    run(['cmake', '-S', ROOT, '-B', build])


def usb_devices(ids):
    import usb.core
    return list(usb.core.find(find_all=True, idVendor=ids[0], idProduct=ids[1]) or [])


def newest_package():
    candidates = sorted(p for p in ROOT.glob('out/anne-package-*') if p.is_dir())
    return candidates[-1] if candidates else None


def latest_record():
    records = sorted((ROOT / 'out/anne-device-private').glob('kernel-install-*'))
    return records[-1] if records else None


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, default=DEFAULT_BUILD,
                        help=f'build directory (default: {DEFAULT_BUILD.relative_to(ROOT)})')
    parser.add_argument('--output', type=Path, default=None,
                        help='package directory (default: out/anne-package-<UTC timestamp>)')
    parser.add_argument('--avb-key', type=Path, default=DEFAULT_KEY,
                        help=f'AVB signing key (default: {DEFAULT_KEY})')
    parser.add_argument('--skip-build', action='store_true', help='reuse the existing build output')
    parser.add_argument('--reuse-latest', action='store_true',
                        help='flash the newest existing out/anne-package-* instead of packaging')
    parser.add_argument('--dry-run', action='store_true',
                        help='build, package and verify local inputs; never touch USB')
    parser.add_argument('--backup', action='store_true',
                        help='capture the previous kernel before writing')
    parser.add_argument('--no-boot', action='store_true',
                        help='leave the RAM LK active after readback')
    args = parser.parse_args()

    ensure_deps()

    if not args.skip_build and not args.reuse_latest:
        refresh_renderer(args.build)
        run(['cmake', '--build', args.build,
             '--target', 'anko', 'mochi_system', 'provenance'])

    package = args.output
    if args.reuse_latest:
        package = newest_package()
        if package is None:
            sys.exit('no out/anne-package-* directory to reuse; run without --reuse-latest')
        print(f'Reusing package {package}', flush=True)
    elif package is None:
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        package = ROOT / 'out' / f'anne-package-{stamp}'

    if not args.reuse_latest:
        if package.exists():
            sys.exit(f'{package} already exists and the packager refuses to overwrite it. '
                     'Pass a fresh --output, or --reuse-latest to flash it as-is.')
        run([venv_python(), 'scripts/anne/build-native-package.py',
             '--build', args.build, '--output', package, '--avb-key', args.avb_key])

    flash = [venv_python(), 'scripts/anne/flash-el2.py', '--package', package]

    if args.dry_run:
        run([*flash, '--check'])
        print(f'\nDry run complete; package is ready at {package}', flush=True)
        print('Re-run with no arguments to flash it.', flush=True)
        return

    candidates = usb_devices(FASTBOOT)
    if candidates:
        serial = candidates[0].serial_number
        print(f'RAM LK already enumerated (serial {serial}); skipping BootROM upload.', flush=True)
        flash.append('--running')
    else:
        if not usb_devices(BOOTROM):
            sys.exit('No ANE in BootROM mode (12d1:3609) and no RAM LK fastboot device.\n'
                     'Put the handset in BootROM download mode, then re-run.')
        module = load_flash_module()
        _, manifest, blobs = module.inputs(package, FIRMWARE)
        print('Uploading xloader, TEE preload and RAM LK through BootROM...', flush=True)
        module.enter_lk(blobs, manifest)
        found = usb_devices(FASTBOOT)
        if not found:
            sys.exit('RAM LK did not enumerate as 18d1:d00d')
        serial = found[0].serial_number
        print(f'RAM LK serial discovered: {serial}', flush=True)
        flash.append('--running')

    flash += ['--serial', serial]
    if args.backup:
        flash.append('--backup')
    if args.no_boot:
        flash.append('--no-boot')

    run(flash)

    record = latest_record()
    if record:
        print(f'\nFlash record: {record}', flush=True)
        result = record / 'result.json'
        if result.exists():
            report = json.loads(result.read_text())
            print(json.dumps(report, indent=2), flush=True)
            if not report.get('readback_verified'):
                sys.exit('readback was not verified')
    print('Done.', flush=True)


if __name__ == '__main__':
    main()

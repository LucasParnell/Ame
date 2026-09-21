#!/usr/bin/env python3
"""Boot the libc lavapipe executor probe in Linux (not the native session bridge)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
MARKERS = ('vkd: real Vulkan device ready', 'vkd: fill/copy fence readback passed',
           'vkd: bounds/reuse/cleanup passed', 'vkd: acceptance passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'build/dango-linux-gpu/Image')
    parser.add_argument('--artifacts', type=Path, default=ROOT / 'build/dango-vkd')
    parser.add_argument('--memory', type=int, default=512, help='standalone Linux RAM in MiB')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    archive = args.artifacts / 'initramfs.cpio.gz'
    if not args.kernel.is_file() or not archive.is_file():
        parser.error('build the GPU Linux kernel and scripts/linux/build-vkd.sh first')
    if args.memory <= 0 or args.timeout <= 0:
        parser.error('memory and timeout must be positive')
    work = args.output or Path(tempfile.mkdtemp(prefix='ame-vkd-'))
    work.mkdir(parents=True, exist_ok=True)
    log = work / 'guest.log'
    command = [os.environ.get('QEMU_SYSTEM_AARCH64', 'qemu-system-aarch64'),
               '-machine', 'virt', '-cpu', 'cortex-a53', '-m', str(args.memory),
               '-smp', '1', '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
               '-no-reboot', '-kernel', str(args.kernel), '-initrd', str(archive),
               '-append', 'console=ttyAMA0 loglevel=4 rdinit=/usr/bin/vkd-probe panic=-1']
    start = time.monotonic()
    reason = 'timeout'
    with log.open('wb') as stream:
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        try:
            while time.monotonic() - start < args.timeout:
                text = log.read_text(errors='replace')
                if any(word in text for word in ('TEST FAILED', 'Kernel panic', 'Out of memory')):
                    reason = 'guest failure'
                    break
                if all(marker in text for marker in MARKERS):
                    reason = 'executor acceptance passed'
                    break
                if process.poll() is not None:
                    reason = f'qemu exited: {process.returncode}'
                    break
                time.sleep(0.1)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    text = log.read_text(errors='replace')
    checks = {marker: marker in text for marker in MARKERS}
    success = reason == 'executor acceptance passed' and all(checks.values())
    result = dict(success=success, reason=reason, elapsed=time.monotonic() - start,
                  checks=checks, log=str(log), scope='standalone Linux executor; native N/O bridge not exercised')
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

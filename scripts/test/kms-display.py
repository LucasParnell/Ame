#!/usr/bin/env python3
"""Phase F acceptance: a real Linux KMS modeset, asserted from the host.

Boots Ame with the SMMUv3 enforced and the assigned virtio-gpu-pci function,
lets the guest appliance userspace drive a standalone KMS modeset (raw DRM
ioctls, no libdrm -- linux-artifacts/kms_test/), captures the QEMU display with
a monitor `screendump`, and asserts the four-colour bar pattern the guest drew.

This proves device assignment + MMIO + IRQ + DMA + Linux DRM + virtio_gpu
independently of the Midix GPU protocol, with Ame containing no virtio-gpu
register logic.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# Loadable by path (host tests) as well as directly; keep the sibling import.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logfmt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# Four equal vertical bars, the pattern kms_test.c fills: XRGB8888 R/G/B/W.
BARS = ((0.125, (255, 0, 0)), (0.375, (0, 255, 0)),
        (0.625, (0, 0, 255)), (0.875, (255, 255, 255)))
TOLERANCE = 24

# edid=on + xres/yres gives the headless QEMU console an EDID with a known
# preferred mode; id=gpu0 lets screendump target this device rather than ramfb.
GPU_DEVICE = 'virtio-gpu-pci,iommu_platform=on,id=gpu0,edid=on,xres=1024,yres=768'


def read_ppm(path):
    data = Path(path).read_bytes()
    tokens = []
    i, n = 0, len(data)
    while len(tokens) < 4:
        while i < n and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < n and data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while j < n and not data[j:j + 1].isspace():
            j += 1
        tokens.append(data[i:j])
        i = j
    i += 1  # single whitespace after maxval
    magic, width, height, maxval = tokens
    if magic != b'P6' or int(maxval) != 255:
        raise ValueError(f'unsupported PPM header: {tokens}')
    return int(width), int(height), data[i:]


def assert_pattern(path):
    width, height, pixels = read_ppm(path)
    if len(pixels) < width * height * 3:
        return False, {'error': 'short pixel buffer'}
    y = height // 2
    samples = {}
    ok = True
    for xf, expect in BARS:
        x = int(width * xf)
        o = (y * width + x) * 3
        got = (pixels[o], pixels[o + 1], pixels[o + 2])
        samples[f'{xf:.3f}'] = list(got)
        if any(abs(a - b) > TOLERANCE for a, b in zip(got, expect)):
            ok = False
    return ok, {'size': [width, height], 'samples': samples}


def evidence(logs):
    match = re.search(r'gpu proof: faults=(\d+) quarantines=(\d+) routes=(\d+) injected=(\d+) smmu=(\d+)', logs)
    counters = dict(zip(('faults', 'quarantines', 'routes', 'injected', 'smmu'),
                        map(int, match.groups()))) if match else {}
    checks = {
        'smmu_ready': 'smmuv3 bring-up rc=0 ready=1' in logs,
        'assignment_active': 'assign: ACTIVE' in logs,
        'linux_driver': 'dango: DRM card0 driver=virtio_gpu dumb_buffer=1' in logs,
        'linux_isr': 'dango: Linux virtio ISR observed' in logs,
        'kms_displayed': 'dango: kms displayed' in logs,
        'kms_passed': 'dango: kms test passed' in logs,
        'no_failures': not any(word in logs for word in (
            'KERNEL PANIC', 'TEST FAILED', 'gpu probe FAILED', 'kms test FAILED',
            'iommu/faults:', 'unsupported trap', 'init: service start failed')),
        'contained_dma': bool(counters) and counters['faults'] == 0 and
                         counters['quarantines'] == 0 and counters['smmu'] == 1,
    }
    return checks, counters


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=str(ROOT / 'out/qemu-gpu'))
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--output')
    args = parser.parse_args()
    work = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='ame-kms-'))
    work.mkdir(parents=True, exist_ok=True)
    monitor = work / 'monitor.sock'
    shot = work / 'kms.ppm'
    env = dict(os.environ, AME_QEMU_IOMMU='1', AME_QEMU_TESTDEV='0', AME_QEMU_GPU_DEVICE=GPU_DEVICE)
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', args.build,
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', 'all services ready', '--marker', 'gpu proof:',
               '--marker', 'dango: kms displayed',
               '--monitor-socket', str(monitor), '--screendump', str(shot),
               '--screendump-device', 'gpu0']
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=args.timeout + 60)
    try:
        boot = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        boot = {'success': False, 'reason': result.stderr or result.stdout}
    logs = ''.join((work / name).read_text(errors='replace') for name in ('host.log', 'guest.log')
                   if (work / name).exists())
    logs = logfmt.marker_stream(logs)
    checks, counters = evidence(logs)
    checks['boot_ready'] = result.returncode == 0 and boot.get('success', False)
    checks['screendump'] = boot.get('screendump_ok') is True and shot.exists()
    pattern = {}
    if checks['screendump']:
        try:
            ok, pattern = assert_pattern(shot)
            checks['kms_pattern'] = ok
        except (OSError, ValueError) as error:
            checks['kms_pattern'] = False
            pattern = {'error': str(error)}
    else:
        checks['kms_pattern'] = False
    success = all(checks.values())
    summary = dict(success=success, reason=boot.get('reason'), elapsed=boot.get('elapsed'),
                   logs=str(work), display=str(shot), checks=checks, counters=counters,
                   pattern=pattern)
    (work / 'kms-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

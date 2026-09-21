#!/usr/bin/env python3
"""Panic-screen acceptance: capture the kernel panic screen from a host screendump.

Builds a dev-only kernel (ANKO_PANIC_SCREEN_DEMO=ON) that raises a real panic as
soon as the framebuffer is bound, boots it with a QEMU monitor, captures the
display with `screendump`, and asserts the screen shows the logo art, the panic
headline and the detail line -- proving the whole panic-screen path end to end.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

# The palette kernel/panic_screen.cc paints with (XRGB8888 -> RGB).
BG = (0, 6, 14)
HEADLINE = (255, 107, 107)
ART_DIM = (143, 166, 196)
ART_BRIGHT = (234, 241, 255)
TOLERANCE = 24


def read_ppm(path):
    data = Path(path).read_bytes()
    tokens, i, n = [], 0, len(data)
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
    i += 1
    magic, width, height, maxval = tokens
    if magic != b'P6' or int(maxval) != 255:
        raise ValueError(f'unsupported PPM header: {tokens}')
    return int(width), int(height), data[i:]


def near(px, color):
    return all(abs(a - b) <= TOLERANCE for a, b in zip(px, color))


def analyze(path):
    width, height, pixels = read_ppm(path)
    total = width * height
    if len(pixels) < total * 3:
        return {'error': 'short pixel buffer'}
    counts = {'bg': 0, 'headline': 0, 'art_dim': 0, 'art_bright': 0}
    for idx in range(total):
        o = idx * 3
        px = (pixels[o], pixels[o + 1], pixels[o + 2])
        if near(px, BG):
            counts['bg'] += 1
        elif near(px, HEADLINE):
            counts['headline'] += 1
        elif near(px, ART_BRIGHT):
            counts['art_bright'] += 1
        elif near(px, ART_DIM):
            counts['art_dim'] += 1
    counts['size'] = [width, height]
    counts['total'] = total
    counts['painted'] = total - counts['bg']
    return counts


def configure_and_build(build, timeout=900):
    args = ['cmake', '-S', str(ROOT), '-B', str(build), '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Debug', '-DANKO_BOARD=qemu-virt', '-DANKO_BUILD_DEV=ON',
            '-DDANGO_BUILD_LINUX_ARTIFACTS=OFF', '-DANKO_PANIC_SCREEN_DEMO=ON']
    if not (build / 'CMakeCache.txt').exists():
        args.append('-DCMAKE_TOOLCHAIN_FILE=scripts/build/toolchain-aarch64.cmake')
    subprocess.run(args, check=True, capture_output=True, timeout=timeout)
    subprocess.run(['cmake', '--build', str(build), '--target', 'anko', 'mochi_system'],
                   check=True, capture_output=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=str(ROOT / 'out/qemu-panic'))
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--output')
    args = parser.parse_args()
    build = Path(args.build)
    work = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='ame-panic-'))
    work.mkdir(parents=True, exist_ok=True)
    monitor = work / 'monitor.sock'
    shot = work / 'panic.ppm'

    configure_and_build(build)

    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', str(build),
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', 'KERNEL PANIC: system halted', '--allow-kernel-panic',
               '--monitor-socket', str(monitor), '--screendump', str(shot)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout + 90)
    try:
        boot = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        boot = {'success': False, 'reason': result.stderr or result.stdout}
    logs = ''.join((work / name).read_text(errors='replace') for name in ('host.log', 'guest.log')
                   if (work / name).exists())
    checks = {
        'panic_raised': 'panic: panic screen demo' in logs and 'KERNEL PANIC: system halted' in logs,
        'boot_ready': result.returncode == 0 and bool(boot.get('success', False)),
        'screendump': boot.get('screendump_ok') is True and shot.exists(),
    }
    counts = {}
    if checks['screendump']:
        try:
            counts = analyze(shot)
            checks['headline'] = counts.get('headline', 0) > 200
            checks['logo_art'] = counts.get('art_dim', 0) + counts.get('art_bright', 0) > 5000
            checks['full_frame'] = counts.get('size') == [1024, 768]
            checks['background'] = counts.get('painted', 0) > 0
        except (OSError, ValueError) as error:
            counts = {'error': str(error)}
            checks.update(headline=False, logo_art=False, full_frame=False, background=False)
    else:
        checks.update(headline=False, logo_art=False, full_frame=False, background=False)
    success = all(checks.values())
    summary = dict(success=success, reason=boot.get('reason'), elapsed=boot.get('elapsed'),
                   logs=str(work), display=str(shot), checks=checks, counts=counts)
    (work / 'panic-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

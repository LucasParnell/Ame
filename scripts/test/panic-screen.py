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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logfmt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
import qr_decode  # noqa: E402

# The palette kernel/panic_screen.cc paints with (XRGB8888 -> RGB).",
BG = (0, 6, 14)
HEADLINE = (255, 107, 107)
REASON = (255, 217, 160)
CAPTION = (200, 214, 232)
ART_DIM = (143, 166, 196)
ART_BRIGHT = (234, 241, 255)
QR_DARK = (8, 17, 28)      # QR module (dark ink on a light panel)
QR_LIGHT = (244, 247, 255)  # QR panel + quiet zone


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
    # Exact match: the renderer writes palette colours directly, and two palette
    # entries are close enough that a tolerance would conflate them.
    return px == color


def decode_qr_from_frame(pixels, width, height, tables):
    """Sample the on-screen QR out of the captured frame and decode it."""
    xs, ys = [], []
    for y in range(height):
        for x in range(width):
            o = (y * width + x) * 3
            if (pixels[o], pixels[o + 1], pixels[o + 2]) == QR_DARK:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    span = x1 - x0 + 1
    for size in range(21, 178, 4):
        if span % size:
            continue
        module = span // size
        if module < 1:
            continue
        rows = []
        ok = True
        for my in range(size):
            row = []
            for mx in range(size):
                o = ((y0 + my * module + module // 2) * width
                     + (x0 + mx * module + module // 2)) * 3
                px = (pixels[o], pixels[o + 1], pixels[o + 2])
                if px == QR_DARK:
                    row.append('1')
                elif px == QR_LIGHT:
                    row.append('0')
                else:
                    ok = False
                    break
            if not ok:
                break
            rows.append(''.join(row))
        if not ok:
            continue
        try:
            return qr_decode.decode(rows, tables)
        except AssertionError:
            continue
    return None


def analyze(path):
    width, height, pixels = read_ppm(path)
    total = width * height
    if len(pixels) < total * 3:
        return {'error': 'short pixel buffer'}
    counts = {'bg': 0, 'qr_dark': 0, 'qr_light': 0, 'headline': 0, 'reason': 0,
              'caption': 0, 'art': 0}
    for idx in range(total):
        o = idx * 3
        px = (pixels[o], pixels[o + 1], pixels[o + 2])
        if near(px, BG):
            counts['bg'] += 1
        elif near(px, QR_DARK):
            counts['qr_dark'] += 1
        elif near(px, QR_LIGHT):
            counts['qr_light'] += 1
        elif near(px, HEADLINE):
            counts['headline'] += 1
        elif near(px, REASON):
            counts['reason'] += 1
        elif near(px, CAPTION):
            counts['caption'] += 1
        elif near(px, ART_DIM) or near(px, ART_BRIGHT):
            counts['art'] += 1
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
    monitor = (work / 'monitor.sock').resolve()
    # A QEMU monitor unix socket path must stay under 104 bytes (boot.py resolves
    # it), so fall back to a short temp directory for a deep output path.
    if len(str(monitor)) >= 100:
        monitor = (Path(tempfile.mkdtemp(prefix='ame-mon-')) / 'm.sock').resolve()
    shot = work / 'panic.ppm'

    configure_and_build(build)

    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', str(build),
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', 'system halted', '--allow-kernel-panic',
               '--monitor-socket', str(monitor), '--screendump', str(shot)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout + 90)
    try:
        boot = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        boot = {'success': False, 'reason': result.stderr or result.stdout}
    logs = ''.join((work / name).read_text(errors='replace') for name in ('host.log', 'guest.log')
                   if (work / name).exists())
    checks = {
        'panic_raised': 'panic screen demo' in logs and 'system halted' in logs,
        'serial_report': 'esr_el1=' in logs and 'call trace (' in logs and 'capability denials=' in logs,
        'boot_ready': result.returncode == 0 and bool(boot.get('success', False)),
        'screendump': boot.get('screendump_ok') is True and shot.exists(),
    }
    counts = {}
    if checks['screendump']:
        try:
            counts = analyze(shot)
            checks['qr_code'] = counts.get('qr_dark', 0) > 200
            checks['headline'] = counts.get('headline', 0) > 200
            checks['reason'] = counts.get('reason', 0) > 50
            checks['key_data'] = counts.get('caption', 0) > 1000
            checks['logo_art'] = counts.get('art', 0) > 5000
            checks['full_frame'] = counts.get('size') == [1024, 768]
            checks['background'] = counts.get('painted', 0) > 0
            # The strongest check: the QR on screen really decodes, and carries
            # the report text.
            width, height, pixels = read_ppm(shot)
            tables = qr_decode.load_tables(ROOT / 'kernel/assets/qr_tables.json')
            decoded = decode_qr_from_frame(pixels, width, height, tables)
            if decoded is None:
                checks['qr_decodes'] = False
            else:
                text = decoded['payload'].decode('ascii', 'replace')
                counts['qr_version'] = decoded['version']
                counts['qr_level'] = decoded['level']
                counts['qr_text_lines'] = text.count('\n')
                checks['qr_decodes'] = (text.startswith('KERNEL PANIC')
                                        and 'reason=' in text and 'x0=' in text
                                        and 'bt_n=' in text)
        except (OSError, ValueError) as error:
            counts = {'error': str(error)}
            checks.update(qr_code=False, headline=False, reason=False, key_data=False,
                          logo_art=False, full_frame=False, background=False,
                          qr_decodes=False)
    else:
        checks.update(qr_code=False, headline=False, reason=False, key_data=False,
                      logo_art=False, full_frame=False, background=False,
                      qr_decodes=False)
    success = all(checks.values())
    summary = dict(success=success, reason=boot.get('reason'), elapsed=boot.get('elapsed'),
                   logs=str(work), display=str(shot), checks=checks, counts=counts)
    (work / 'panic-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

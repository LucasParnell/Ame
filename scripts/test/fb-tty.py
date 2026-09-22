#!/usr/bin/env python3
"""Phase J acceptance: the *tty frame* through the appliance.

Boots Ame with the assigned virtio-gpu-pci function and the SMMUv3 enforced,
lets `tty.fb0` reach the shell (`msh` prints its banner to the terminal), and
asserts two things the `gpud` row does not:

  * the frame `io.framebuffer` renders reaches the appliance: the GPU service
    logs `gpu service: present ok` only after the guest GPU server has completed
    the native frame's present; and
  * that frame is *readable tty text*: the captured frame is decoded cell by
    cell against the shared VGA console font
    (`shared/include/midix/font/vga8x16.h`) and must carry the shell banner the
    runtime prints (`userland/msh/main.cc`), not merely the bootstrap bar
    pattern the `gpud` row asserts.

Capture target: the assigned GPU's QEMU console, selected by its qdev id.
A native framebuffer capture cannot prove that an appliance present succeeded.

The PPM reader and the screendump boot runner are Phase F/G's (kms-display.py /
boot.py), imported rather than copied, so every appliance row agrees on the wire
and the capture mechanism.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# Loadable by path (host tests) as well as directly; keep the sibling import.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ef_constants as ef  # noqa: E402
import logfmt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# Glyph shapes and the shell banner are single-sourced in the tree, never
# restated here: the console font the framebuffer blits and the banner the shell
# prints to `tty.fb0`.
FONT_HEADER = ROOT / 'shared/include/midix/font/vga8x16.h'
MSH_MAIN = ROOT / 'userland/msh/main.cc'

# A readable text screen carries at least this many non-blank glyph cells; the
# shell banner alone is ~50, so this only rejects a near-blank / non-text frame.
MIN_TEXT_CELLS = 200


def _load_kms_display():
    """Import scripts/test/kms-display.py by path for its PPM helpers."""
    path = Path(__file__).with_name('kms-display.py')
    spec = importlib.util.spec_from_file_location('kms_display', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


kms = _load_kms_display()


def load_console_font(path=FONT_HEADER):
    """Parse the shared 8x16 console font header into {code: (row bytes...)}.

    The header is generated (scripts/fonts/psf2_to_header.py) and is the single
    home of the glyph shapes, so the decoder reads it rather than carrying a
    second copy of the font. Only the `/* 0xNN */ { 0x.., ... }` rows are read.
    """
    rows = re.findall(r'/\* 0x([0-9A-Fa-f]{2}) \*/\s*\{([^}]*)\}', Path(path).read_text())
    if len(rows) != 256:
        raise ValueError(f'{path}: expected 256 glyphs, parsed {len(rows)}')
    font = {}
    for hex_code, body in rows:
        values = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
        if len(values) != 16:
            raise ValueError(f'{path}: glyph 0x{hex_code} has {len(values)} rows, expected 16')
        font[int(hex_code, 16)] = tuple(values)
    return font


def shell_banner_literals(path=MSH_MAIN):
    """The literal strings msh prints to the terminal at startup.

    Read from the shell's own source so the expected tty text is never a second
    copy of the banner; any one of them appearing in the decoded frame proves the
    terminal carried real shell text.
    """
    literals = re.findall(r'putln\("((?:[^"\\]|\\.)*)"\)', Path(path).read_text())
    banner = [lit.encode().decode('unicode_escape') for lit in literals if lit.startswith('msh:')]
    if not banner:
        raise ValueError(f'{path}: no "msh: ..." putln() banner literal found')
    return banner


def decode_screen(width, height, pixels, font):
    """Decode an XRGB8888 PPM body into text using the console font.

    The framebuffer blits each glyph cell as 1-bit `fg` on `bg`; the background
    is the frame's dominant colour (the terminal fill), so a cell pixel that is
    not the background is an "on" bit. A cell whose bits match a font glyph
    decodes to that character; an all-blank cell is a space; an unmatched cell is
    reported as `?` and counted, so a frame that is not the console font at all
    cannot masquerade as text.
    """
    if len(pixels) < width * height * 3:
        raise ValueError('short pixel buffer')
    colours = {}
    for i in range(0, min(len(pixels), width * height * 3), 3):
        key = pixels[i:i + 3]
        colours[key] = colours.get(key, 0) + 1
    background = max(colours, key=colours.get)
    by_glyph = {}
    for code, rows in font.items():
        by_glyph.setdefault(rows, code)

    lines = []
    text_cells = 0
    unknown = 0
    for cell_y in range(height // 16):
        line = []
        for cell_x in range(width // 8):
            bits = []
            for row in range(16):
                byte = 0
                for col in range(8):
                    o = ((cell_y * 16 + row) * width + cell_x * 8 + col) * 3
                    if pixels[o:o + 3] != background:
                        byte |= 0x80 >> col
                bits.append(byte)
            glyph = tuple(bits)
            if glyph == by_glyph.get(font.get(0x20, (0,) * 16)) or not any(glyph):
                line.append(' ')
                continue
            code = by_glyph.get(glyph)
            text_cells += 1
            if code is None:
                unknown += 1
                line.append('?')
            else:
                line.append(chr(code) if 32 <= code < 127 else '?')
        lines.append(''.join(line).rstrip())
    return lines, {'text_cells': text_cells, 'unknown_glyphs': unknown,
                   'background': list(background)}


def evidence(logs):
    counters = ef.parse_proof(logs, 'gpu')
    checks = {
        'smmu_ready': ef.MARKERS['smmu_ready'] in logs,
        'linux_driver': ef.MARKERS['linux_driver'] in logs,
        'control_hello': ef.MARKERS['gpu_control_hello'] in logs,
        'staging_registered': ef.MARKERS['gpu_staging_registered'] in logs,
        'present_complete': ef.MARKERS['gpu_present_complete'] in logs,
        # The framebuffer armed its client and tty.fb0 reached the shell.
        'fb_gpu_armed': ef.MARKERS['fb_gpu_armed'] in logs,
        'tty_shell': ef.MARKERS['tty_shell'] in logs,
        # The tty frame crossed the service boundary: the GPU service wrote the
        # client's frame into the appliance scanout buffer.
        'frame_staged': ef.MARKERS['gpu_frame_staged'] in logs,
        'frame_presented': ef.MARKERS['gpu_frame_presented'] in logs,
        'no_failures': not any(word in logs for word in ef.FAILURE_WORDS),
        'iommu_faults_zero': bool(counters) and counters['faults'] == 0 and
                             counters['quarantines'] == 0,
    }
    return checks, counters


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=str(ROOT / 'out/qemu-gpu'))
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--output')
    args = parser.parse_args()
    work = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='ame-fbtty-'))
    work.mkdir(parents=True, exist_ok=True)
    monitor = work / 'monitor.sock'
    shot = work / 'tty.ppm'
    env = dict(os.environ, AME_QEMU_IOMMU='1', AME_QEMU_TESTDEV='0',
               AME_QEMU_GPU_DEVICE=kms.GPU_DEVICE)
    # Require completed native-frame presentation and capture the assigned GPU.
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', args.build,
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', ef.MARKERS['boot_ready'],
               '--marker', ef.MARKERS['gpu_present_complete'],
               '--marker', ef.MARKERS['tty_shell'],
               '--marker', ef.MARKERS['gpu_frame_presented'],
               '--monitor-socket', str(monitor), '--screendump', str(shot),
               '--screendump-device', ef.GPU_QDEV_ID]
    result = subprocess.run(command, env=env, capture_output=True, text=True,
                            timeout=args.timeout + 60)
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
    decode = {}
    if checks['screendump']:
        try:
            width, height, pixels = kms.read_ppm(shot)
            lines, decode = decode_screen(width, height, pixels, load_console_font())
            text = '\n'.join(lines)
            banner = [b for b in shell_banner_literals() if b in text]
            checks['tty_readable'] = (decode['text_cells'] >= MIN_TEXT_CELLS and
                                      len(banner) > 0)
            decode['size'] = [width, height]
            decode['banner_matched'] = banner
            decode['tail'] = [line for line in lines if line.strip()][-4:]
        except (OSError, ValueError) as error:
            checks['tty_readable'] = False
            decode = {'error': str(error)}
    else:
        checks['tty_readable'] = False
    success = all(checks.values())
    summary = dict(success=success, reason=boot.get('reason'), elapsed=boot.get('elapsed'),
                   logs=str(work), display=str(shot), checks=checks, counters=counters,
                   tty=decode)
    (work / 'fb-tty-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compare isolated centering calls while retaining the real firmware scratch buffer."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile


SPEC = importlib.util.spec_from_file_location('anne_unicorn', Path(__file__).with_name('emulate-fastboot.py'))
emulation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(emulation)


def analyse(image, output, first=(2, 1), second=(8, 8), panel=(8, 8), bpp=1):
    if not all(0 < w <= panel[0] and 0 < h <= panel[1] for w, h in (first, second)):
        raise ValueError('both bitmaps must fit the panel')
    data = image.read_bytes()
    emulator = emulation.FastbootEmulator(data)
    entry = emulator.bmp_profile(first, panel, bpp)
    if output is None:
        output = Path(tempfile.mkdtemp(prefix='ame-fastboot-bmp-'))
    else:
        output = output.expanduser().resolve()
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    runs = []
    for index, bitmap in enumerate((first, second), start=1):
        if index == 2:
            emulator.prepare_bitmap(bitmap, bpp)
            emulator.assumptions.append(f'Second isolated call with {bitmap[0]}x{bitmap[1]} '
                                        'decoded pixels; allocator cursor and scratch pointer retained.')
        before = emulator.heap.violation_count
        with (output / f'call-{index}-events.jsonl').open('w') as events:
            report = emulator.run(entry, count=100000, timeout_ms=5000, events=events)
        report['experiment'] = {'bitmap': bitmap, 'panel': panel, 'bytes_per_pixel': bpp,
                                'new_bounds_violations': emulator.heap.violation_count - before,
                                'scope': 'isolated centering with synthetic decoded pixels and display state'}
        (output / f'call-{index}-report.json').write_text(json.dumps(report, indent=2) + '\n')
        pixels = bytes(emulator.uc.mem_read(0x35000000, panel[0] * panel[1] * bpp))
        (output / f'call-{index}-pixels.bin').write_bytes(pixels)
        runs.append({'call': index, 'bitmap': bitmap, 'stop_reason': report['stop_reason'],
                     'return_value': report['registers']['x0'], 'cursor': report['heap']['cursor'],
                     'scratch_pointer': report['heap']['scratch_pointer'],
                     'allocation_count': len(report['heap']['allocations']),
                     'new_bounds_violations': report['experiment']['new_bounds_violations'],
                     'pixel_sha256': hashlib.sha256(pixels).hexdigest()})
        if report['stop_reason'] != 'returned':
            break
    emulation.save_allocations(output, emulator.heap.allocations)
    summary = {'schema': 1, 'image': str(image.resolve()), 'image_sha256': emulator.sha256,
               'scope': 'isolated function calls; not boot-time heap reconstruction or a BMP parser test',
               'assumptions': emulator.assumptions, 'runs': runs,
               'source_image_unchanged': hashlib.sha256(image.read_bytes()).hexdigest() == emulator.sha256}
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return output, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=emulation.DEFAULT_IMAGE)
    parser.add_argument('--first', type=emulation.dimensions, default=(2, 1))
    parser.add_argument('--second', type=emulation.dimensions, default=(8, 8))
    parser.add_argument('--panel', type=emulation.dimensions, default=(8, 8))
    parser.add_argument('--bytes-per-pixel', type=int, default=1)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        output, summary = analyse(args.image.expanduser(), args.output, args.first,
                                  args.second, args.panel, args.bytes_per_pixel)
        for run in summary['runs']:
            print(f"Call {run['call']}: {run['bitmap']}, {run['stop_reason']}, "
                  f"allocations={run['allocation_count']}, cursor={run['cursor']:#x}, "
                  f"new bounds violations={run['new_bounds_violations']}")
        print(f'Results: {output}')
        return 0 if len(summary['runs']) == 2 and all(r['stop_reason'] == 'returned' for r in summary['runs']) else 1
    except (OSError, ValueError, emulation.unicorn.UcError) as error:
        parser.exit(2, f'BMP centering analysis: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Benchmark immutable DTB discovery; host timings do not predict MMU-off ARM.

Pass --baseline-dir with saved probe.cc/fdt.cc to compare an earlier parser.
Output includes all samples, their median, and a semantic hardware hash.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb', type=Path)
    parser.add_argument('--baseline-dir', type=Path)
    parser.add_argument('--iterations', type=int, default=2000)
    parser.add_argument('--samples', type=int, default=7)
    args = parser.parse_args()
    if args.iterations <= 0 or args.samples <= 0:
        parser.error('iterations and samples must be positive')
    sources = {'optimized': [ROOT / 'kernel/platform/probe.cc', ROOT / 'kernel/init/fdt.cc']}
    if args.baseline_dir:
        sources['baseline'] = [args.baseline_dir / 'probe.cc', args.baseline_dir / 'fdt.cc']
    report = {'dtb_bytes': args.dtb.stat().st_size, 'timing': 'host, warm cache', 'parsers': {}}
    with tempfile.TemporaryDirectory(prefix='ame-dtb-benchmark-') as directory:
        programs = {}
        for name, paths in sources.items():
            program = Path(directory) / name
            subprocess.run(['clang++', '-std=c++23', '-O3',
                            '-I' + str(ROOT / 'kernel/include'),
                            '-I' + str(ROOT / 'shared/include'), '-idirafter', str(ROOT / 'lib/include'),
                            str(ROOT / 'tests/platform_benchmark.cc'), *map(str, paths),
                            str(ROOT / 'lib/libk/string.cc'), '-o', str(program)], check=True)
            programs[name] = program
            report['parsers'][name] = {'samples_us': []}
        # Interleave runs so gradual host load/temperature changes affect both.
        for _ in range(args.samples):
            for name, program in programs.items():
                sample = json.loads(subprocess.check_output(
                    [str(program), str(args.dtb.resolve()), str(args.iterations)], text=True))
                result = report['parsers'][name]
                result['samples_us'].append(sample['mean_us'])
                result['hardware_hash'] = sample['hardware_hash']
        for result in report['parsers'].values():
            result['median_us'] = statistics.median(result['samples_us'])
        if len(programs) == 2:
            optimized, baseline = (report['parsers'][name] for name in ('optimized', 'baseline'))
            if optimized['hardware_hash'] != baseline['hardware_hash']:
                raise RuntimeError('baseline and optimized hardware descriptions differ')
            report['speedup'] = baseline['median_us'] / optimized['median_us']
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Compare cached host checksum workloads; timings do not predict device speed."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def measure(source, work, runs, optimization):
    binary = work / 'probe'
    subprocess.run([
        'clang++', '-std=c++23', optimization,
        '-I' + str(source / 'shared/include'), '-idirafter', str(source / 'lib/include'),
        str(ROOT / 'tests/integrity_benchmark.cc'), str(source / 'shared/format/integrity.cc'),
        '-o', str(binary)], check=True)
    rows = {}
    for _ in range(runs):
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=60)
        for line in result.stdout.splitlines():
            row = dict(field.split('=') for field in line.split())
            rows.setdefault(row['case'], []).append(float(row['ns_per_byte']))
    return {key: {'samples_ns_per_byte': values, 'median_ns_per_byte': statistics.median(values)}
            for key, values in rows.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-dir', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--debug', action='store_true')
    args = parser.parse_args()
    if args.runs < 1:
        parser.error('--runs must be positive')
    with tempfile.TemporaryDirectory(prefix='ame-integrity-benchmark-') as directory:
        work = Path(directory)
        old, new = work / 'baseline', work / 'current'
        old.mkdir(); new.mkdir()
        optimization = '-O0' if args.debug else '-O3'
        baseline = measure(args.baseline_dir.resolve(), old, args.runs, optimization)
        current = measure(ROOT, new, args.runs, optimization)
        print(json.dumps({'kind': 'cached host integrity workload', 'optimization': optimization,
                         'baseline': baseline, 'current': current,
                         'speedup': {key: baseline[key]['median_ns_per_byte'] / row['median_ns_per_byte']
                                     for key, row in current.items()}}, indent=2))


if __name__ == '__main__':
    main()

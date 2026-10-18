#!/usr/bin/env python3
"""Warm host dispatch benchmark; hardware boundaries use the scheduler test fixture.

An optional snapshot contains kernel/sched/*.cc, sched_internal.h,
kernel/include/anko/{sched,object}.h, kernel/cap/*.cc, and the regression probe
when its context layout differs. Both versions run the same
workload: 200,000 dispatches, 1/8/32 runnable threads, and 1,024 unrelated live
object records, plus concurrent CPU accounting of a shared principal, with and
without a shared budgeted context. These are host stress timings, not phone times.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from test_scheduler import build_probe


def measure(work, snapshot, runs):
    work.mkdir()
    probe = build_probe(work, snapshot=snapshot, benchmark=True, run=False)
    samples = {}
    for _ in range(runs):
        result = subprocess.run([str(probe)], check=True, capture_output=True,
                                text=True, timeout=30)
        for line in result.stdout.splitlines():
            row = dict(field.split('=') for field in line.split())
            timing = 'accounting_ns' if 'accounting_ns' in row else 'dispatch_ns'
            group = samples.setdefault(row.get('case', row['threads']), {
                'samples_ns': [], 'principal_bytes': int(row['principal_bytes']),
                'station_bytes': int(row['station_bytes']),
                'timing': timing,
            })
            group['samples_ns'].append(float(row[timing]))
    for group in samples.values():
        group['median_ns'] = statistics.median(group['samples_ns'])
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-dir', type=Path)
    parser.add_argument('--runs', type=int, default=5)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error('--runs must be positive')
    with tempfile.TemporaryDirectory(prefix='ame-scheduler-benchmark-') as directory:
        work = Path(directory)
        output = {'kind': 'warm host dispatch stress benchmark'}
        if args.baseline_dir:
            output['baseline'] = measure(work / 'baseline', args.baseline_dir.resolve(), args.runs)
        output['current'] = measure(work / 'current', None, args.runs)
        if args.baseline_dir:
            output['speedup'] = {count: output['baseline'][count]['median_ns'] / row['median_ns']
                                 for count, row in output['current'].items()}
        print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()

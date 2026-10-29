#!/usr/bin/env python3
"""Validate a CDC diag-log capture of bench.ipc.loan and save its paired results."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from benchmarks.ipc.loan_results import parse_log, summarize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    args = parser.parse_args()
    runs = parse_log(args.capture.read_text())
    paths = [ROOT / p for p in ('benchmarks/ipc/loans.cc', 'benchmarks/ipc/workload.h',
                               'userland/lib/include/mochi/loan.h', 'userland/lib/include/mochi/app_loan.h',
                               'userland/lib/include/mochi/framed_channel.h')]
    if args.image:
        paths.append(args.image)
    report = {'schema': 'ame.ipc-loans/v1', 'runs': runs, 'summary': summarize(runs),
              'capture_sha256': hashlib.sha256(args.capture.read_bytes()).hexdigest(),
              'current_file_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
              'contract': '4 KiB; generate/validate each byte; loan RPC returns the same buffer; '
                          'stream returns ownership per window; timings include ownership bookkeeping',
              'limits': 'Separate experiment, not copied IPC v1 or Linux comparison. '
                        'Medians of per-launch round medians, not independent boot results. '
                        'Current file hashes must be checked against the measured artifact.'}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = ['# Native 4 KiB shared-buffer loans', '',
             '| Mode | Copied control | Shared loans | Less time |',
             '| --- | ---: | ---: | ---: |']
    for row in report['summary']:
        line = (f'| {row["mode"]} | {row["copied_ring"]["median_ns"] / 1000:.2f} µs | '
                f'{row["shared_loan"]["median_ns"] / 1000:.2f} µs | '
                f'{row["loan_time_reduction_percent"]:.1f}% |')
        lines.append(line)
        print(line)
    lines += ['', f'{len(runs)} completed runs, each with all four cases and full byte validation.',
              '', report['contract'], '', report['limits']]
    (args.output / 'results.md').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()

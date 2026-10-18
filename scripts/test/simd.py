#!/usr/bin/env python3
"""Run the real userland SIMD scheduler workload at EL1/EL2 on QEMU."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import boot

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'out/qemu-dev')
    parser.add_argument('--cpus', type=int, nargs='+', choices=(1, 2, 4, 8), default=[1, 2, 4, 8])
    parser.add_argument('--entry-el', type=int, nargs='+', choices=(1, 2), default=[1, 2])
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    work = args.output or Path(tempfile.mkdtemp(prefix='ame-simd-matrix-'))
    work.mkdir(parents=True, exist_ok=True)
    build = args.build.resolve()
    result = subprocess.run(['cmake', '--build', str(build), '--target', 'anko', 'simd_regressions'],
                            capture_output=True, text=True)
    (work / 'build.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        print(json.dumps({'success': False, 'reason': 'build failed', 'logs': str(work)}))
        return 1
    rows = []
    for cpus in args.cpus:
        for el in args.entry_el:
            passed = boot.run(build, 'direct', cpus, args.timeout,
                              bundle=build / 'simd.mxb', entry_el=el, cpu='cortex-a53',
                              markers=['userland SIMD regression passed'],
                              output=work / f'cpus-{cpus}-el{el}')
            rows.append({'cpus': cpus, 'entry_el': el, 'success': passed})
    summary = {'success': all(row['success'] for row in rows), 'cases': rows, 'logs': str(work)}
    (work / 'simd-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))
    return 0 if summary['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

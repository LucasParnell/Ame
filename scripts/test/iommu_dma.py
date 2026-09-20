#!/usr/bin/env python3
"""Phase C SMMUv3 hostile-DMA verification (spec §8.14).

Boots Ame under QEMU with a machine-wide SMMUv3, no default PCI-bus IOMMU
bypass, and the `iommu-testdev` reference device, then asserts that the kernel's
opt-in selftest actually proved:

  (a) DMA to a mapped appliance IOVA succeeds;
  (b) DMA to an unmapped IOVA faults, IOMMU_FAULTS advances, and the canary page
      is unchanged;
  (c) a read-only mapping rejects a device write;
  (d) a requester detached from its domain faults rather than bypassing;
  (e) repeated faults cannot overflow the event queue and quarantine the stream.

The topology is opt-in (AME_QEMU_IOMMU=1 AME_QEMU_TESTDEV=1), so the default
scripts/verify.sh gate is unaffected.  Prints a JSON summary and exits non-zero
on any failure.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run_boot(build, cpus, timeout, work):
    env = dict(os.environ)
    env['AME_QEMU_IOMMU'] = '1'
    env['AME_QEMU_TESTDEV'] = '1'
    cmd = [sys.executable, str(ROOT / 'scripts/test/boot.py'),
           '--build', str(build), '--mode', 'direct', '--cpus', str(cpus),
           '--timeout', str(timeout), '--output', str(work),
           '--marker', 'iommu dma test passed', '--marker', 'all services ready']
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout + 60)
    try:
        result = json.loads(proc.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        result = {'success': False, 'reason': f'runner output: {proc.stdout!r} {proc.stderr!r}'}
    return result


def read_logs(work):
    text = ''
    for name in ('host.log', 'guest.log'):
        path = pathlib.Path(work) / name
        if path.exists():
            text += path.read_text(errors='replace')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=str(ROOT / 'out/verify'),
                        help='build directory to boot (default: out/verify)')
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--output', help='keep QEMU logs in this directory')
    args = parser.parse_args()

    work = pathlib.Path(args.output) if args.output else pathlib.Path(
        subprocess.run(['mktemp', '-d', '-t', 'ame-iommu-'], capture_output=True,
                       text=True).stdout.strip())

    result = run_boot(args.build, args.cpus, args.timeout, work)
    logs = read_logs(work)

    checks = {
        'boot_ready': result.get('success', False),
        'passed_marker': 'iommu dma test passed' in logs,
        'no_test_failure': 'TEST FAILED' not in logs,
        '(a) mapped': '(a) mapped DMA ok result=0x0' in logs,
        '(b) unmapped': '(b) unmapped fault ok' in logs and 'canary=0xa5a5a5a5' in logs,
        '(c) read-only': '(c) read-only rejection ok' in logs,
        '(d) detached': '(d) detached fault ok' in logs,
        '(e) quarantine': '(e) repeated faults ok' in logs and 'state=3' in logs,
        '(e) no queue overflow': 'gerror=0x0' in logs,
        '(f) unmap+tlbi': '(f) unmap + scoped TLBI ok' in logs,
    }
    counters = {}
    match = re.search(
        r'counters IOMMU_FAULTS=(\d+) IOMMU_QUARANTINES=(\d+)(?: IOMMU_TLBI_RANGE=(\d+))? '
        r'events=(\d+)', logs)
    if match:
        counters = {'faults': int(match.group(1)), 'quarantines': int(match.group(2)),
                    'tlbi_range': int(match.group(3) or 0), 'events': int(match.group(4))}
        checks['faults_counter'] = counters['faults'] > 0
        checks['quarantine_counter'] = counters['quarantines'] > 0
        checks['tlbi_range_counter'] = counters['tlbi_range'] > 0
    else:
        checks['faults_counter'] = False

    success = all(checks.values())
    summary = dict(success=success, reason=result.get('reason'), elapsed=result.get('elapsed'),
                   logs=str(work), checks=checks, counters=counters,
                   evidence=[line for line in logs.splitlines()
                             if 'iommu/smmuv3' in line or 'iommu/selftest' in line
                             or 'iommu/faults' in line or 'iommu/native' in line])
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

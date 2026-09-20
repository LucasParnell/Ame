#!/usr/bin/env python3
"""Phase D device-assignment verification (spec §6, §7, §8.11, §8.12, §8.14).

Boots Ame under QEMU with a machine-wide SMMUv3, no default PCI-bus IOMMU
bypass, and the `iommu-testdev` reference device (the same opt-in topology the
Phase C hostile-DMA test uses), then asserts the kernel's opt-in assignment
proof actually demonstrated:

  (i)   the host function is NOT bus-mastering when the secure-attach machine
        completes (a forced BME bit was cleared and never re-enabled without a
        mediated guest write);
  (ii)  after a mediated guest BME write, DMA through the assignment's IOMMU
        domain succeeds for a mapped IOVA while an unmapped IOVA faults;
  (iii) at revoke, host bus-master is cleared and DMA is rejected with the
        canary page intact;
  (iv)  a guest MSI/MSI-X request produces a vGIC pending interrupt, and a level
        route suppresses-after-inject and re-arms on guest EOI.

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
           '--marker', 'device assign test passed', '--marker', 'all services ready']
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
        subprocess.run(['mktemp', '-d', '-t', 'ame-assign-'], capture_output=True,
                       text=True).stdout.strip())

    result = run_boot(args.build, args.cpus, args.timeout, work)
    logs = read_logs(work)

    checks = {
        'boot_ready': result.get('success', False),
        'passed_marker': 'device assign test passed' in logs,
        'no_test_failure': 'TEST FAILED' not in logs,
        'attach_contained': 'assign: contained bm=0 fns=1/1' in logs,
        '(i) not_mastering': '(i) contained bm=0 state=active' in logs,
        '(ii) bar_device': '(ii) bar mapped' in logs and 'device=1' in logs,
        '(ii) mapped_dma': '(ii) mapped DMA ok result=0x0' in logs,
        '(ii) unmapped_fault': '(ii) unmapped fault ok result=0xdead0002 canary=0xa5a5a5a5' in logs,
        '(iii) revoke': '(iii) revoke bm=0 dma=rejected' in logs and
                        'canary=0xa5a5a5a5' in logs and 'state=revoked' in logs,
        '(iv) msi_pending': '(iv) msi route injected vector=42 pending=1' in logs,
        '(iv) level_rearm': '(iv) level route ok deliveries=1->2' in logs,
    }
    counters = {}
    match = re.search(r'counters assign_created=(\d+) active=(\d+) bar=(\d+) routes=(\d+) '
                      r'injected=(\d+) deny=(\d+) bme=(\d+) iommu_faults=(\d+)', logs)
    if match:
        counters = {
            'assign_created': int(match.group(1)), 'active': int(match.group(2)),
            'bar_mapped': int(match.group(3)), 'irq_routes': int(match.group(4)),
            'irq_injected': int(match.group(5)), 'mediated_deny': int(match.group(6)),
            'bme_enabled': int(match.group(7)), 'iommu_faults': int(match.group(8)),
        }
        checks['created'] = counters['assign_created'] >= 1
        checks['active'] = counters['active'] >= 1
        checks['bar_mapped'] = counters['bar_mapped'] >= 1
        checks['irq_injected'] = counters['irq_injected'] >= 1
        checks['mediated_deny'] = counters['mediated_deny'] >= 2
        checks['faults_seen'] = counters['iommu_faults'] >= 1
    else:
        checks['created'] = False

    success = all(checks.values())
    summary = dict(success=success, reason=result.get('reason'), elapsed=result.get('elapsed'),
                   logs=str(work), checks=checks, counters=counters,
                   evidence=[line for line in logs.splitlines()
                             if 'assign:' in line or 'revoke:' in line or 'vm/selftest' in line])
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

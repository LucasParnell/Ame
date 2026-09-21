#!/usr/bin/env python3
"""Phase E acceptance: real Linux DRM probe behind SMMUv3, with INTx delivery."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# The host tests load this file by path from the repo root, so the scripts/test
# directory is not on sys.path automatically; add it for the sibling import.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ef_constants as ef  # noqa: E402
import logfmt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def evidence(logs):
    counters = ef.parse_proof(logs, 'gpu')
    checks = {
        'smmu_ready': ef.MARKERS['smmu_ready'] in logs,
        'assignment_active': ef.MARKERS['assign_active'] in logs,
        'bar_device_mapping': f'assign: bar4 mapped ipa={ef.GPU_BAR4_IPA:#x}' in logs,
        'linux_driver': ef.MARKERS['linux_driver'] in logs,
        'linux_isr': ef.MARKERS['linux_isr'] in logs,
        'guest_probe': ef.MARKERS['gpu_probe'] in logs,
        'no_failures': not any(word in logs for word in ef.FAILURE_WORDS),
        'contained_dma': bool(counters) and counters['faults'] == 0 and
                         counters['quarantines'] == 0 and counters['smmu'] == 1,
        'host_interrupts': bool(counters) and counters['routes'] == 1 and counters['injected'] > 0,
    }
    return checks, counters


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=str(ROOT / 'out/qemu-gpu'))
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--output')
    args = parser.parse_args()
    work = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='ame-gpu-'))
    env = dict(os.environ, AME_QEMU_IOMMU='1', AME_QEMU_TESTDEV='0')
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', args.build,
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', ef.MARKERS['boot_ready'], '--marker', ef.MARKERS['gpu_proof']]
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=args.timeout + 30)
    try:
        boot = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        boot = {'success': False, 'reason': result.stderr or result.stdout}
    logs = ''.join((work / name).read_text(errors='replace') for name in ('host.log', 'guest.log')
                   if (work / name).exists())
    logs = logfmt.marker_stream(logs)
    checks, counters = evidence(logs)
    checks['boot_ready'] = result.returncode == 0 and boot.get('success', False)
    success = all(checks.values())
    summary = dict(success=success, reason=boot.get('reason'), elapsed=boot.get('elapsed'),
                   logs=str(work), checks=checks, counters=counters)
    work.mkdir(parents=True, exist_ok=True)
    (work / 'gpu-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

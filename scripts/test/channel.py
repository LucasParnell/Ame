#!/usr/bin/env python3
"""Phase G acceptance: ordered host/Linux channel traffic with vIRQ and HVC notification."""
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
import logfmt  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def evidence(logs):
    match = re.search(r'channel proof: faults=(\d+) quarantines=(\d+) routes=(\d+) injected=(\d+) smmu=(\d+)', logs)
    counters = dict(zip(('faults', 'quarantines', 'routes', 'injected', 'smmu'),
                        map(int, match.groups()))) if match else {}
    checks = {
        'smmu_ready': 'smmuv3 bring-up rc=0 ready=1' in logs,
        'assignment_active': 'assign: ACTIVE' in logs,
        'bar_device_mapping': 'assign: bar4 mapped ipa=0x50000000' in logs,
        'linux_driver': 'dango: DRM card0 driver=virtio_gpu dumb_buffer=1' in logs,
        'linux_isr': 'dango: Linux virtio ISR observed' in logs,
        'native_channel': 'channel native probe passed' in logs,
        'host_channel': 'channel guest probe passed: 512 ordered round trips' in logs,
        'guest_channel': 'dango: channel test passed' in logs,
        'h2g_count': bool(re.search(r'channel.h2g=512\b', logs)),
        'g2h_count': bool(re.search(r'channel.g2h=514\b', logs)),
        'guest_probe': 'dango: gpu probe passed' in logs,
        'no_failures': not any(word in logs for word in (
            'KERNEL PANIC', 'TEST FAILED', 'channel test FAILED', 'channel guest probe FAILED', 'channel native probe FAILED', 'gpu probe FAILED', 'iommu/faults:',
            'unsupported trap', 'init: service start failed')),
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
    work = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='ame-channel-'))
    env = dict(os.environ, AME_QEMU_IOMMU='1', AME_QEMU_TESTDEV='0')
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', args.build,
               '--cpus', str(args.cpus), '--timeout', str(args.timeout), '--output', str(work),
               '--marker', 'all services ready', '--marker', 'channel proof:',
               '--marker', 'channel guest probe passed: 512 ordered round trips']
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
    (work / 'channel-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())

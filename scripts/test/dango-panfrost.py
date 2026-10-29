#!/usr/bin/env python3
"""Boot the Panfrost appliance in Dango; prove transport, not phone GPU access."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import logfmt


def evidence(logs):
    return {
        'agent': 'dango-agent: started' in logs,
        'native_channel': 'channel native probe passed' in logs,
        'host_channel': 'channel guest probe passed: 512 ordered round trips' in logs,
        'guest_channel': 'dango: channel test passed' in logs,
        'native_ready': 'init: all services ready' in logs,
        'no_failure': not any(word in logs for word in (
            'FAILED', 'KERNEL PANIC', 'USERFAULT', 'unsupported guest trap',
            'vcpu_run syscall error', 'linux load failed')),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'out/dango-panfrost-qemu-dev')
    ap.add_argument('--output', type=Path, default=ROOT / 'out/dango-panfrost-validation')
    ap.add_argument('--timeout', type=float, default=120)
    args = ap.parse_args()
    spec = importlib.util.spec_from_file_location('profile_check', ROOT / 'scripts/linux/check-profile.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    cache = dict(line.split('=', 1) for line in (args.build / 'CMakeCache.txt').read_text().splitlines()
                 if '=' in line and not line.startswith(('//', '#')))
    if cache.get('DANGO_LINUX_PROFILE:STRING') != 'panfrost':
        ap.error('build must package the panfrost profile')
    check.check(cache['DANGO_LINUX_ARTIFACT_DIR:PATH'], 'panfrost')
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', str(args.build),
               '--cpu', 'cortex-a53', '--cpus', '2', '--entry-el', '2',
               '--timeout', str(args.timeout), '--output', str(args.output)]
    for marker in ('channel guest probe passed: 512 ordered round trips', 'dango: channel test passed'):
        command += ['--marker', marker]
    run = subprocess.run(command, env=dict(os.environ, AME_QEMU_IOMMU='0', AME_QEMU_TESTDEV='0'),
                         capture_output=True, text=True, timeout=args.timeout + 30)
    logs = logfmt.marker_stream('\n'.join((args.output / name).read_text(errors='replace')
        for name in ('host.log', 'guest.log') if (args.output / name).exists()))
    checks = evidence(logs)
    checks['bounded_boot'] = run.returncode == 0
    result = dict(success=all(checks.values()), checks=checks,
                  physical_gpu_tested=False, logs=str(args.output))
    if run.returncode:
        result['runner_error'] = run.stderr or run.stdout
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'panfrost-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

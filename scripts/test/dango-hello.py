#!/usr/bin/env python3
"""Prove the signed hello-only Linux guest runs through Dango on Cortex-A53.

Mochi readiness alone precedes guest execution. Acceptance requires the Linux
agent's own UART messages as well as successful VM entry. An EL1 run instead
proves explicit unavailability without executing a guest or an unknown HVC.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import logfmt  # noqa: E402


def counter(logs, name):
    match = re.search(r'(?<![\w.])' + re.escape(name) + r'=(\d+)\b', logs)
    return int(match.group(1)) if match else None


def evidence(logs, entry_el=2):
    common = {
        'entry_level': f'boot_el={entry_el} ' in logs,
        'no_fault': not any(marker in logs for marker in (
            'KERNEL PANIC', 'USERFAULT', 'TEST FAILED', 'unsupported guest trap',
            'vcpu_run syscall error', 'linux load failed', 'guest memory failed')),
    }
    if entry_el == 1:
        return dict(common,
            unavailable=counter(logs, 'vm.el2_available') == 0 and
                'el2 unavailable: dango VM cannot run guest' in logs,
            no_guest='dango-agent: started' not in logs and 'starting linux guest' not in logs)
    return dict(common,
        monitor_ready=counter(logs, 'vm.el2_available') == 1,
        artifacts='linux artifacts ready' in logs,
        vm_created='vm created' in logs,
        entry_returned=counter(logs, 'vcpu.first_rc') == 0,
        agent_started='dango-agent: started' in logs,
        guest_hello='dango: linux guest responded' in logs,
        native_ready='init: all services ready' in logs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'out/dango-hello-qemu-dev')
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4), default=2)
    parser.add_argument('--entry-el', type=int, choices=(1, 2), default=2)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    work = args.output or Path(tempfile.mkdtemp(prefix='ame-dango-hello-'))
    # Inspect the actual artifact directory used by this build, not a global
    # default that could accidentally validate a different signed payload.
    cache = (args.build / 'CMakeCache.txt').read_text()
    directory = re.search(r'^DANGO_LINUX_ARTIFACT_DIR:PATH=(.+)$', cache, re.MULTILINE)
    profile = re.search(r'^DANGO_LINUX_PROFILE:STRING=(.+)$', cache, re.MULTILINE)
    if not directory or not profile or profile.group(1) != 'hello':
        parser.error('build must package the hello Linux artifact profile')
    spec = importlib.util.spec_from_file_location('hello_profile', ROOT / 'scripts/linux/check-profile.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    check.check(directory.group(1), 'hello')
    manifest = check.tomllib.loads((Path(directory.group(1)) / 'manifest.toml').read_text())
    if manifest.get('guest', 'minimal') != 'minimal':
        parser.error('hello acceptance requires the minimal guest')

    env = dict(os.environ, AME_QEMU_IOMMU='0', AME_QEMU_TESTDEV='0')
    markers = (['dango-agent: started', 'dango: linux guest responded',
                'vcpu.first_rc=0', 'init: all services ready'] if args.entry_el == 2 else
               ['el2 unavailable: dango VM cannot run guest'])
    command = [sys.executable, str(ROOT / 'scripts/test/boot.py'), '--build', str(args.build),
               '--cpu', 'cortex-a53', '--cpus', str(args.cpus), '--entry-el', str(args.entry_el),
               '--timeout', str(args.timeout), '--output', str(work)]
    for marker in markers:
        command += ['--marker', marker]
    run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=args.timeout + 30)
    logs = logfmt.marker_stream('\n'.join((work / name).read_text(errors='replace')
        for name in ('host.log', 'guest.log') if (work / name).exists()))
    checks = evidence(logs, args.entry_el)
    checks['bounded_boot'] = run.returncode == 0
    result = dict(success=all(checks.values()), checks=checks, logs=str(work),
                  entry_el=args.entry_el, cpus=args.cpus)
    if run.returncode:
        result['runner_error'] = run.stderr or run.stdout
    work.mkdir(parents=True, exist_ok=True)
    (work / 'hello-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

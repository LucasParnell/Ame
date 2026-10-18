#!/usr/bin/env python3
"""Interleave direct QEMU boots and measure Linux host CPU use after readiness.

CPU seconds are QEMU host process time, not guest profiling or device power.
Compare on an otherwise quiet host; TCG and host scheduling affect both metrics.
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


def cpu_seconds(pid):
    # comm can contain spaces/parentheses; fields after its last ')' start at 3.
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def measure(build, output, cpus, idle_seconds):
    output.mkdir(parents=True, exist_ok=True)
    log = output / 'host.log'
    log.write_text('')
    command = ['qemu-system-aarch64', '-machine', 'virt,virtualization=on,gic-version=2',
               '-cpu', 'cortex-a57', '-smp', str(cpus), '-m', '512M',
               '-device', 'ramfb', '-device', 'qemu-xhci,id=xhci',
               '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-mouse,bus=xhci.0',
               '-display', 'none', '-serial', f'file:{log}', '-serial', 'null',
               '-monitor', 'none', '-kernel', str(build / 'kernel/anko.bin'),
               '-initrd', str(build / 'system.mxb')]
    started = time.monotonic()
    with (output / 'qemu.log').open('w') as errors:
        process = subprocess.Popen(command, stdout=errors, stderr=errors)
        try:
            while True:
                text = log.read_text(errors='replace')
                if process.poll() is not None or 'KERNEL PANIC' in text:
                    raise RuntimeError(f'boot failed; see {output}')
                if 'all services ready' in text:
                    break
                if time.monotonic() - started > 45:
                    raise TimeoutError(f'readiness timeout; see {output}')
                time.sleep(0.05)
            ready = time.monotonic() - started
            # Exclude startup tail work before measuring a fixed idle window.
            time.sleep(0.5)
            cpu_before, idle_start = cpu_seconds(process.pid), time.monotonic()
            while time.monotonic() - idle_start < idle_seconds:
                if process.poll() is not None:
                    raise RuntimeError(f'QEMU exited after readiness; see {output}')
                time.sleep(0.05)
            wall = time.monotonic() - idle_start
            cpu = cpu_seconds(process.pid) - cpu_before
            phases = {key: int(value) for key, value in
                      re.findall(r'(verify_ns|boot.parse_ns|spawn_ns|wiring_ns|boot.launch_ns|longest_wait_ns)=(\d+)', text)}
            return {'ready_seconds': ready, 'idle_host_cpu_seconds_per_second': cpu / wall,
                    'phases_ns': phases, 'logs': str(output)}
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4, 8), default=2)
    parser.add_argument('--idle-seconds', type=float, default=2)
    args = parser.parse_args()
    if args.runs < 1 or not 0 < args.idle_seconds <= 30:
        parser.error('runs must be positive and idle-seconds in (0, 30]')
    args.output.mkdir(parents=True, exist_ok=True)
    samples = {'baseline': [], 'current': []}
    for index in range(args.runs):
        # Alternate order so warmup/host drift do not always favor one build.
        order = ('baseline', 'current') if index % 2 == 0 else ('current', 'baseline')
        for name in order:
            build = args.baseline_build if name == 'baseline' else args.build
            samples[name].append(measure(build.resolve(), args.output / f'{name}-{index}',
                                         args.cpus, args.idle_seconds))
    result = {'kind': __doc__, 'cpus': args.cpus, 'samples': samples}
    result['medians'] = {name: {key: statistics.median(row[key] for row in rows)
                               for key in ('ready_seconds', 'idle_host_cpu_seconds_per_second')}
                         for name, rows in samples.items()}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

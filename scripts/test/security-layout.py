#!/usr/bin/env python3
"""Boot three isolated QEMU instances and verify kernel/user layout variation.

Addresses are collected only from this regression's private logs and TCG trace;
production kernels do not export KASLR addresses. No disks/devices are modified.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import time


def run(build: Path, output: Path, index: int, weak: bool = False):
    directory = output / str(index); directory.mkdir(parents=True, exist_ok=True)
    serial, trace = directory/'host.log', directory/'instructions.log'
    command = [shutil.which('qemu-system-aarch64'), '-machine', 'virt,virtualization=off,gic-version=2' + (',dtb-randomness=off' if weak else ''),
        '-cpu', 'cortex-a53', '-smp', '1', '-m', '512M', '-display', 'none',
        '-serial', 'file:'+str(serial), '-monitor', 'none', '-d', 'in_asm', '-D', str(trace),
        '-kernel', str(build/'kernel/anko.bin'), '-initrd', str(build/'lifecycle.mxb')]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic()+45
        while time.monotonic() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if 'TEST FAILED' in text or 'KERNEL PANIC' in text: raise RuntimeError(text[-4000:])
            if 'lifecycle regression passed' in text: break
            if process.poll() is not None: raise RuntimeError('QEMU exited before validation')
            time.sleep(.05)
        else: raise RuntimeError('QEMU layout regression timed out')
    finally:
        process.terminate()
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        process.stderr.close()
    expected = ('uptime seed fallback; KASLR/ASLR active with weak entropy' if weak else
                'trusted entropy; kernel TTBR1 and process ASLR active')
    if expected not in text: raise RuntimeError('expected entropy source/randomization not active')
    user = re.search(r'ASLR fixture entry/stack/heap: (0x[0-9a-f]+) (0x[0-9a-f]+) (0x[0-9a-f]+)', text)
    if not user: raise RuntimeError('missing actual user layout fixture')
    code, stack, heap = (int(value, 16) for value in user.groups())
    if not (0x11000000 <= code < 0x18000000 and 0x3e000000 <= stack < 0x40000000 and
            0x3c000000 <= heap < 0x3e000000):
        raise RuntimeError('randomized layout escaped its ABI window')
    kernel = None
    with trace.open(errors='replace') as stream:
        for line in stream:
            match = re.match(r'0x(ffff8[0-9a-f]+):', line)
            if match: kernel = int(match[1], 16) & ~0x1fffff; break
    if kernel is None: raise RuntimeError('no high-half kernel instructions observed')
    # Bound artifact size; disassembly served its one validation purpose.
    trace.unlink()
    return {'kernel': kernel, **dict(zip(('code', 'stack', 'heap'), map(lambda s:int(s, 16), user.groups())))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--weak-fallback', action='store_true', help='disable firmware RNG seed and validate the uptime fallback')
    args = parser.parse_args(); build = args.build.resolve(); output = args.output.resolve()
    layouts = [run(build, output, index, args.weak_fallback) for index in range(3)]
    variation = {region: len({layout[region] for layout in layouts}) > 1
                 for region in ('kernel', 'code', 'stack', 'heap')}
    if not args.weak_fallback:
        for region, varied in variation.items():
            if not varied: raise RuntimeError(f'{region} layout did not vary')
    result = {'success':True, 'boots':3, 'entropy':'weak uptime' if args.weak_fallback else 'trusted firmware', 'variation':variation, 'regions':['kernel TTBR1', 'user code/data', 'stack', 'primary arena']}
    (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))

if __name__ == '__main__': main()

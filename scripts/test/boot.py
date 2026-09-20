#!/usr/bin/env python3
"""Bounded QEMU acceptance runner; never writes the supplied disk image.

The disk is attached with a discarded overlay (snapshot=on) by default, so a
boot cannot mutate the image it was given. Pass --no-snapshot only when the
caller already owns a disposable copy of the image (e.g. the durability
regression in scripts/test/durable-persistence.py).
"""
import argparse
import json
import pathlib
import subprocess
import tempfile
import time


def run(build, mode, cpus, timeout, disk=None, firmware=None, markers=None, output=None,
        bundle=None, snapshot=True):
    build = pathlib.Path(build).resolve()
    output = pathlib.Path(output) if output else pathlib.Path(tempfile.mkdtemp(prefix='ame-boot-'))
    output.mkdir(parents=True, exist_ok=True)
    host, guest = output / 'host.log', output / 'guest.log'
    args = ['qemu-system-aarch64', '-machine', 'virt,virtualization=on,gic-version=2',
            '-cpu', 'cortex-a57', '-smp', str(cpus), '-m', '512M', '-device', 'ramfb',
            '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
            '-device', 'usb-mouse,bus=xhci.0', '-display', 'none', '-monitor', 'none',
            '-serial', f'file:{host}', '-serial', f'file:{guest}']
    if disk:
        snapshot_arg = 'snapshot=on' if snapshot else 'snapshot=off'
        args += ['-drive',
                 f'if=none,file={pathlib.Path(disk).resolve()},format=raw,id=disk,{snapshot_arg}',
                 '-device', 'virtio-blk-device,drive=disk']
    if mode == 'direct':
        args += ['-kernel', str(build / 'kernel/anko.bin'), '-initrd', str(pathlib.Path(bundle).resolve() if bundle else build / 'system.mxb')]
    else:
        if not firmware or not disk:
            raise ValueError('disk boot requires --firmware and --disk')
        args += ['-bios', str(pathlib.Path(firmware).resolve())]
    markers = markers or ['system ready']
    success = False
    reason = 'timeout'
    with (output / 'qemu.log').open('w') as errors:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=errors, stderr=errors)
        started = time.monotonic()
        try:
            while time.monotonic() - started < timeout:
                logs = '\n'.join(p.read_text(errors='replace') if p.exists() else '' for p in [host, guest])
                if any(s in logs for s in ['KERNEL PANIC', 'spawn failed', 'init: manifest failed', 'TEST FAILED', 'invalid or incompatible BootInfo']):
                    reason = 'guest failure'; break
                if all(s in logs for s in markers):
                    success = True; reason = 'ready'; break
                if proc.poll() is not None:
                    reason = f'qemu exited {proc.returncode}'; break
                time.sleep(0.1)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    result = dict(success=success, reason=reason, mode=mode, cpus=cpus,
                  elapsed=time.monotonic() - started, logs=str(output), command=args,
                  snapshot=snapshot)
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    return success


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    parser.add_argument('--mode', choices=['direct', 'disk'], default='direct')
    parser.add_argument('--cpus', type=int, choices=[1, 2, 4], default=2)
    parser.add_argument('--timeout', type=float, default=45)
    parser.add_argument('--disk')
    parser.add_argument('--bundle')
    parser.add_argument('--firmware')
    parser.add_argument('--output')
    parser.add_argument('--marker', action='append', dest='markers')
    parser.add_argument('--no-snapshot', action='store_true',
                        help='give the guest a writable disk (default uses a discarded overlay)')
    args = parser.parse_args()
    params = vars(args)
    params['snapshot'] = not params.pop('no_snapshot')
    return 0 if run(**params) else 1


if __name__ == '__main__':
    raise SystemExit(main())

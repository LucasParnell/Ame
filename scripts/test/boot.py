#!/usr/bin/env python3
"""Bounded QEMU acceptance runner; never writes the supplied disk image.

The disk is attached with a discarded overlay (snapshot=on) by default, so a
boot cannot mutate the image it was given. Pass --no-snapshot only when the
caller already owns a disposable copy of the image (e.g. the durability
regression in scripts/test/durable-persistence.py).
"""
import argparse
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

# The host tests load this file by path from the repo root, so the scripts/test
# directory is not on sys.path automatically; add it for the sibling import.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logfmt  # noqa: E402


def hmp_screendump(sock_path, out_path, device=None, timeout=10.0):
    """Capture the QEMU display to a PPM through the HMP monitor socket.

    The monitor is attached with `-monitor unix:<path>,server,nowait`, so no
    host display is required; QEMU's graphic console still exists under
    `-display none`. `device` selects the console by qdev id when several
    display devices are attached (e.g. ramfb plus an assigned virtio-gpu).
    """
    deadline = time.monotonic() + timeout
    while not os.path.exists(sock_path) and time.monotonic() < deadline:
        time.sleep(0.05)
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(timeout)
    try:
        conn.connect(sock_path)
        try:
            conn.recv(4096)  # banner + first prompt
        except (socket.timeout, OSError):
            pass
        command = f'screendump {out_path}' + (f' {device}' if device else '') + '\n'
        conn.sendall(command.encode())
        buf = b''
        while time.monotonic() < deadline:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if b'(qemu)' in buf:
                break
    finally:
        conn.close()
    while time.monotonic() < deadline:
        if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
            return True
        time.sleep(0.05)
    return False


def run(build, mode, cpus, timeout, disk=None, firmware=None, markers=None, output=None,
        bundle=None, snapshot=True, monitor_socket=None, screendump=None,
        screendump_device=None, screendump_delay=0.5, allow_kernel_panic=False):
    build = pathlib.Path(build).resolve()
    output = pathlib.Path(output) if output else pathlib.Path(tempfile.mkdtemp(prefix='ame-boot-'))
    output.mkdir(parents=True, exist_ok=True)
    host, guest = output / 'host.log', output / 'guest.log'
    # Opt-in Phase A topology: attach an SMMUv3 (no default bus bypass) and a
    # virtio-gpu-pci display function, while keeping ramfb.  The default gate
    # topology below stays byte-for-byte unchanged unless the env opts in.
    # AME_QEMU_TESTDEV=1 additionally attaches the QEMU iommu-testdev reference
    # device used by the Phase C hostile-DMA verification.
    iommu = os.environ.get('AME_QEMU_IOMMU') == '1'
    testdev = os.environ.get('AME_QEMU_TESTDEV') == '1'
    machine = 'virt,virtualization=on,gic-version=2'
    if iommu:
        machine += ',iommu=smmuv3,default-bus-bypass-iommu=off'
    args = ['qemu-system-aarch64', '-machine', machine,
            '-cpu', 'cortex-a57', '-smp', str(cpus), '-m', '512M', '-device', 'ramfb',
            '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0',
            '-device', 'usb-mouse,bus=xhci.0', '-display', 'none',
            '-serial', f'file:{host}', '-serial', f'file:{guest}']
    if monitor_socket:
        args += ['-monitor', f'unix:{pathlib.Path(monitor_socket).resolve()},server,nowait']
    else:
        args += ['-monitor', 'none']
    if iommu:
        args += ['-device', os.environ.get('AME_QEMU_GPU_DEVICE',
                                           'virtio-gpu-pci,iommu_platform=on')]
    if testdev:
        args += ['-device', 'iommu-testdev']
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
    failures = ['spawn failed', 'init: manifest failed', 'TEST FAILED', 'gpu probe FAILED',
                'kms test FAILED', 'channel test FAILED', 'channel guest probe FAILED',
                'channel native probe FAILED', 'invalid or incompatible BootInfo']
    if not allow_kernel_panic:
        failures.append('KERNEL PANIC')
    success = False
    reason = 'timeout'
    screen_ok = None
    # Clear previous evidence before Popen: QEMU may not open its serial files
    # before our first poll, especially when a caller reuses --output.
    for log in (host, guest):
        log.write_text('')
    with (output / 'qemu.log').open('w') as errors:
        proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=errors, stderr=errors)
        started = time.monotonic()
        try:
            while time.monotonic() - started < timeout:
                raw = '\n'.join(p.read_text(errors='replace') if p.exists() else '' for p in [host, guest])
                # Ame's own log entries can split a guest line; match markers on
                # the raw stream (Ame markers) plus the de-interleaved guest view.
                logs = logfmt.marker_stream(raw)
                if any(s in logs for s in failures):
                    reason = 'guest failure'; break
                if all(s in logs for s in markers):
                    success = True; reason = 'ready'
                    if screendump:
                        time.sleep(screendump_delay)
                        screen_ok = hmp_screendump(monitor_socket, screendump, screendump_device)
                    break
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
                  snapshot=snapshot, iommu=iommu, testdev=testdev,
                  screendump=screendump, screendump_ok=screen_ok)
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
    parser.add_argument('--allow-kernel-panic', action='store_true',
                        help='a kernel panic is the expected outcome (panic-screen capture)')
    parser.add_argument('--monitor-socket',
                        help='attach a HMP monitor on this unix socket (enables screendump)')
    parser.add_argument('--screendump', help='capture the display to this PPM once markers match')
    parser.add_argument('--screendump-device', help='qdev id of the display device to capture')
    parser.add_argument('--screendump-delay', type=float, default=0.5,
                        help='settle time between marker match and capture (seconds)')
    parser.add_argument('--no-snapshot', action='store_true',
                        help='give the guest a writable disk (default uses a discarded overlay)')
    args = parser.parse_args()
    params = vars(args)
    params['snapshot'] = not params.pop('no_snapshot')
    return 0 if run(**params) else 1


if __name__ == '__main__':
    raise SystemExit(main())

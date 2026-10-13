#!/usr/bin/env python3
"""Exercise the actual xHCI -> HID -> input chain with QEMU keyboard events.

Requires a development bundle built with MOCHI_USB_TRACE_INPUT=ON. No disk or
physical USB device is touched. Multiple keys guard against losing re-arm after
the first report; modifier and release markers prove more than enumeration.
"""
import argparse
import os
import pathlib
import socket
import tempfile
import threading
import time

import boot


def run(build, output, cpus=2, reconnect=0):
    output = pathlib.Path(output).resolve()
    finished = threading.Event()
    failures = []
    with tempfile.TemporaryDirectory(prefix='ame-usb-monitor-') as work:
        monitor = pathlib.Path(work) / 'monitor.sock'

        def inject():
            deadline = time.monotonic() + 45
            log = output / 'host.log'
            while not finished.is_set() and time.monotonic() < deadline:
                text = log.read_text(errors='replace') if log.exists() else ''
                if monitor.exists() and 'drv.usb.hid: keyboard ready' in text and 'ui.input: ready' in text:
                    break
                finished.wait(0.02)
            else:
                if not finished.is_set():
                    failures.append('USB services did not become ready')
                return
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(2)
                    connection.connect(str(monitor))
                    connection.recv(4096)
                    for key in ('a', 'b', 'shift-c'):
                        connection.sendall(f'sendkey {key} 80\n'.encode())
                        if finished.wait(0.25):
                            return
                    def await_log(predicate, label):
                        until = time.monotonic() + 15
                        while not finished.is_set() and time.monotonic() < until:
                            if predicate(log.read_text(errors='replace')):
                                return True
                            finished.wait(0.02)
                        if not finished.is_set():
                            failures.append(label)
                        return False
                    for cycle in range(reconnect):
                        if not await_log(lambda value: 'usb.hid.key_up=6' in value,
                                         'Initial reports did not finish'):
                            return
                        before = log.read_text(errors='replace')
                        detached = before.count('drv.usb.hid: keyboard detached')
                        ready = before.count('drv.usb.hid: keyboard ready')
                        connection.sendall(b'device_del ame-usb-test-kbd\n')
                        if not await_log(lambda value: value.count('drv.usb.hid: keyboard detached') > detached,
                                         'Keyboard detach was not observed'):
                            return
                        connection.sendall(b'device_add usb-kbd,id=ame-usb-test-kbd,bus=xhci.0\n')
                        if not await_log(lambda value: value.count('drv.usb.hid: keyboard ready') > ready,
                                         'Keyboard reconnect did not enumerate'):
                            return
                        key, code = ('d', 7) if cycle == 0 else ('e', 8)
                        connection.sendall(f'sendkey {key} 80\n'.encode())
                        if not await_log(lambda value: f'usb.hid.key_up={code}' in value,
                                         'Reconnected keyboard did not produce a report'):
                            return
            except OSError as error:
                failures.append(str(error))

        # Remove the previous log before the injection watcher starts. boot.run
        # also enforces fresh captures, but its cleanup runs on the main thread.
        output.mkdir(parents=True, exist_ok=True)
        (output / 'host.log').write_text('')
        worker = threading.Thread(target=inject, daemon=True)
        worker.start()
        markers = ['drv.usb.hid: keyboard ready', 'init: all services ready']
        for key in (4, 5, 6, 0xe1):
            markers.extend((f'usb.hid.key_down={key}', f'usb.hid.key_up={key}'))
        for key in range(7, 7 + reconnect):
            markers.extend((f'usb.hid.key_down={key}', f'usb.hid.key_up={key}'))
        try:
            success = boot.run(build, 'direct', cpus, 60, output=output,
                               markers=markers, monitor_socket=monitor,
                               usb_keyboard_id='ame-usb-test-kbd' if reconnect else None)
        finally:
            finished.set()
            worker.join(timeout=3)
        if failures:
            raise RuntimeError('; '.join(failures))
        return success


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', default='out/usb-input-validation')
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4), default=2)
    parser.add_argument('--reconnect', type=int, choices=(0, 1, 2), default=0)
    parser.add_argument("--iommu",action="store_true",help="Exercise routed xHCI DMA with SMMUv3 enabled")
    args = parser.parse_args()
    if args.iommu: os.environ["AME_QEMU_IOMMU"]="1"
    return 0 if run(args.build, args.output, args.cpus, args.reconnect) else 1


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Exercise the actual xHCI -> HID -> input chain with QEMU keyboard events.

Requires a development bundle built with MOCHI_USB_TRACE_INPUT=ON. No disk or
physical USB device is touched. Multiple keys guard against losing re-arm after
the first report; modifier and release markers prove more than enumeration.
"""
import argparse
import pathlib
import socket
import tempfile
import threading
import time

import boot


def run(build, output, cpus=2):
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
                if monitor.exists() and 'drv.usb.xhci: keyboard ready' in text and 'ui.input: ready' in text:
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
            except OSError as error:
                failures.append(str(error))

        # Remove the previous log before the injection watcher starts. boot.run
        # also enforces fresh captures, but its cleanup runs on the main thread.
        output.mkdir(parents=True, exist_ok=True)
        (output / 'host.log').write_text('')
        worker = threading.Thread(target=inject, daemon=True)
        worker.start()
        markers = ['drv.usb.xhci: keyboard ready', 'init: all services ready']
        for key in (4, 5, 6, 0xe1):
            markers.extend((f'usb.hid.key_down={key}', f'usb.hid.key_up={key}'))
        try:
            success = boot.run(build, 'direct', cpus, 60, output=output,
                               markers=markers, monitor_socket=monitor)
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
    args = parser.parse_args()
    return 0 if run(args.build, args.output, args.cpus) else 1


if __name__ == '__main__':
    raise SystemExit(main())

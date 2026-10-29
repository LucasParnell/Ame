#!/usr/bin/env python3
"""Reboot a running Ame handset, optionally waiting for fastboot.

Ame's shell owns power transitions: ``reboot fastboot`` (alias
``reboot bootloader``) asks init to reset with the fastboot boot reason. The
command travels over the development CDC serial port, the same transport as
``scripts/anne/usb-input.py console``.

Use this when the phone is running Ame and you want to flash: reboot to
fastboot here, then hand over to ``scripts/anne/fastboot.py``.
"""

from __future__ import annotations

import argparse
import glob
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fastboot as fb  # noqa: E402

PORT_PATTERNS = ("/dev/cu.usbmodemanne_native_*", "/dev/cu.usbmodem*")


def console_port(explicit: "str | None") -> str:
    if explicit:
        return explicit
    for pattern in PORT_PATTERNS:
        matches = sorted(glob.glob(pattern))
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise SystemExit(
                f"several CDC ports match {pattern}; pass --port: " + ", ".join(matches))
    raise SystemExit("no Ame CDC serial port; is the phone running Ame?")


def send(port: str, command: str, read_seconds: float = 3.0) -> str:
    arguments = [sys.executable, str(HERE / "usb-input.py")]
    if port:
        arguments += ["--serial", port]
    arguments += ["console", "--send", command, "--enter", "--read", str(read_seconds)]
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=20)
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0 or "reboot requested" not in output:
        raise SystemExit(f"reboot was not acknowledged by Ame:\n{output.strip()}")
    return output


def wait_for_fastboot(fastboot: str, serial: "str | None", timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            devices = fb.list_devices(fastboot)
        except Exception:
            devices = []
        if serial and serial in devices:
            return serial
        if not serial and devices:
            return devices[0]
        time.sleep(0.5)
    wanted = "the target device" if serial else "any fastboot device"
    raise SystemExit(f"{wanted} did not appear within {timeout:.0f}s")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--to", choices=("fastboot", "system"), default="fastboot")
    parser.add_argument("--port", help="CDC serial port (default: auto-detect)")
    parser.add_argument("--serial", help="expected fastboot serial after the reboot")
    parser.add_argument("--fastboot", help="fastboot binary used to poll for the device")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait for fastboot enumeration")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    target = "reboot fastboot" if args.to == "fastboot" else "reboot"
    fastboot = fb.find_fastboot(args.fastboot) if args.to == "fastboot" else None
    serial = args.serial or fb.pinned_serial()
    report = {"action": "reboot", "target": args.to}

    if args.to == "fastboot" and fastboot:
        present = fb.list_devices(fastboot)
        if present and (not serial or serial in present):
            report.update({"already_in_fastboot": True,
                           "serial_fingerprint": fb.fingerprint(serial or present[0])})
            if args.json:
                print(json.dumps(report, indent=2))
            else:
                print("already in fastboot; nothing to do")
            return 0

    port = console_port(args.port)
    report["port"] = port
    send(port, target)
    report["reboot_requested"] = True

    if args.to == "fastboot":
        found = wait_for_fastboot(fastboot, serial, args.timeout)
        report.update({"fastboot_serial_fingerprint": fb.fingerprint(found),
                       "waited": True})
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"sent {target!r} on {port}")
        if args.to == "fastboot":
            print("phone is in fastboot; next: scripts/anne/fastboot.py flash --package DIR --write")
    return 0


if __name__ == "__main__":
    sys.exit(main())

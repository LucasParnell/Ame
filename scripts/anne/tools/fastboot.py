#!/usr/bin/env python3
"""Reset-aware fastboot control for the Ame ANE-LX1 test handset.

Why a USB reset precedes every command
--------------------------------------
On this phone a fastboot command is answered once per USB session; a second
command issued on the same session is not acknowledged reliably. Every
per-run flash script under ``out/anne-device-private/*/`` therefore reset the
USB device (libusb, 18d1:d00d, matched by serial) immediately before each
fastboot invocation. That invariant lives here now.

``devices`` is exempt: it is the enumeration call itself.

Safety
------
Writes are opt-in. ``flash`` and ``restore`` validate the package offline and
print a plan; nothing is sent to the phone unless ``--write`` is given. A real
write always runs a fresh preflight (model, firmware, bootloader lock state)
before the image is handed to fastboot.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SERIAL_FILE = ROOT / "out/anne-device-private/adb-targets.json"
BUNDLED_FASTBOOT = (
    ROOT / "out/anne-device-private/usb-auto-v5/platform-tools/fastboot",
    ROOT / "out/host-tools/platform-tools-35.0.2/fastboot",
)

USB_VID = 0x18D1
USB_PID = 0xD00D
USB_LIBRARY_NAMES = (
    "/opt/homebrew/opt/libusb/lib/libusb-1.0.dylib",
    "/usr/local/opt/libusb/lib/libusb-1.0.dylib",
)

MODEL = "ANE-LX1"
BOARD = "huawei-anne"
PARTITION = "kernel"
KERNEL_PARTITION_BYTES = 24 * 1024 * 1024

IMAGE_KINDS = {
    "native": ("ame-native-kernel.img", "native_image_sha256"),
    "restore": ("restore-stock-kernel.img", "restore_kernel_sha256"),
}


class FastbootError(RuntimeError):
    """A device command was refused, unanswered, or not acknowledged."""


class ResetUnavailable(FastbootError):
    """libusb cannot be loaded, so the reset invariant cannot be honoured."""


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def redact(text: str, serial: "str | None") -> str:
    return text.replace(serial, "<device>") if serial else text


def fingerprint(serial: "str | None") -> "str | None":
    if not serial:
        return None
    return hashlib.sha256(serial.encode()).hexdigest()[:12]


def find_fastboot(explicit: "str | None" = None) -> str:
    for candidate in (explicit, os.environ.get("AME_FASTBOOT")):
        if candidate:
            path = Path(candidate).expanduser()
            if path.is_file() and os.access(path, os.X_OK):
                return str(path)
            raise FastbootError(f"fastboot is not executable: {candidate}")
    for path in BUNDLED_FASTBOOT:
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    found = shutil.which("fastboot")
    if found:
        return found
    raise FastbootError("fastboot not found; pass --fastboot or set AME_FASTBOOT")


def decode(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def list_devices(fastboot: str, run=subprocess.run, timeout: int = 10) -> list:
    result = run([fastboot, "devices"], capture_output=True, text=True, timeout=timeout)
    serials = []
    for line in decode(result.stdout).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "fastboot":
            serials.append(fields[0])
    return serials


def pinned_serial() -> "str | None":
    try:
        entries = json.loads(SERIAL_FILE.read_text())
    except (OSError, ValueError):
        return None
    serials = {entry.get("serial") for entry in entries if isinstance(entry, dict)}
    serials.discard(None)
    return serials.pop() if len(serials) == 1 else None


def resolve_serial(explicit: "str | None", fastboot: str, run=subprocess.run) -> str:
    live = list_devices(fastboot, run=run)
    if explicit:
        return explicit
    if os.environ.get("AME_SERIAL"):
        return os.environ["AME_SERIAL"]
    if len(live) == 1:
        return live[0]
    known = pinned_serial()
    if known and (not live or known in live):
        return known
    if not live:
        raise FastbootError("no fastboot device; connect the phone in fastboot mode")
    raise FastbootError(
        "several fastboot devices present; pass --serial (found "
        + ", ".join(fingerprint(serial) for serial in live)
        + ")"
    )


class UsbReset:
    """Reset the matching 18d1:d00d USB device through libusb."""

    class Descriptor(ctypes.Structure):
        _fields_ = [
            ("length", ctypes.c_uint8),
            ("type", ctypes.c_uint8),
            ("usb", ctypes.c_uint16),
            ("device_class", ctypes.c_uint8),
            ("subclass", ctypes.c_uint8),
            ("protocol", ctypes.c_uint8),
            ("packet", ctypes.c_uint8),
            ("vendor", ctypes.c_uint16),
            ("product", ctypes.c_uint16),
            ("version", ctypes.c_uint16),
            ("manufacturer_index", ctypes.c_uint8),
            ("product_index", ctypes.c_uint8),
            ("serial_index", ctypes.c_uint8),
            ("configurations", ctypes.c_uint8),
        ]

    def __init__(self, serial: str):
        self.serial = serial
        self._library = None

    def _load(self):
        if self._library is not None:
            return self._library
        names = [ctypes.util.find_library("usb-1.0"), *USB_LIBRARY_NAMES]
        for name in names:
            if not name:
                continue
            try:
                library = ctypes.CDLL(name)
            except OSError:
                continue
            pointer = ctypes.c_void_p
            library.libusb_init.argtypes = [ctypes.POINTER(pointer)]
            library.libusb_init.restype = ctypes.c_int
            library.libusb_get_device_list.argtypes = [pointer, ctypes.POINTER(ctypes.POINTER(pointer))]
            library.libusb_get_device_list.restype = ctypes.c_ssize_t
            library.libusb_get_device_descriptor.argtypes = [pointer, ctypes.POINTER(self.Descriptor)]
            library.libusb_get_device_descriptor.restype = ctypes.c_int
            library.libusb_open.argtypes = [pointer, ctypes.POINTER(pointer)]
            library.libusb_open.restype = ctypes.c_int
            library.libusb_get_string_descriptor_ascii.argtypes = [
                pointer, ctypes.c_uint8, ctypes.c_void_p, ctypes.c_int]
            library.libusb_get_string_descriptor_ascii.restype = ctypes.c_int
            library.libusb_reset_device.argtypes = [pointer]
            library.libusb_reset_device.restype = ctypes.c_int
            library.libusb_close.argtypes = [pointer]
            library.libusb_close.restype = None
            library.libusb_free_device_list.argtypes = [ctypes.POINTER(pointer), ctypes.c_int]
            library.libusb_free_device_list.restype = None
            library.libusb_exit.argtypes = [pointer]
            library.libusb_exit.restype = None
            self._library = library
            return library
        raise ResetUnavailable(
            "libusb-1.0 not found (brew install libusb); --no-reset bypasses the "
            "reset invariant and usually leaves the next command unanswered"
        )

    def __call__(self) -> dict:
        library = self._load()
        context = ctypes.c_void_p()
        if library.libusb_init(ctypes.byref(context)) != 0:
            raise ResetUnavailable("libusb_init failed")
        devices = ctypes.POINTER(ctypes.c_void_p)()
        count = library.libusb_get_device_list(context, ctypes.byref(devices))
        result = {"matching_device_reset": False}
        try:
            for index in range(max(0, count)):
                descriptor = self.Descriptor()
                if library.libusb_get_device_descriptor(devices[index], ctypes.byref(descriptor)) != 0:
                    continue
                if (descriptor.vendor, descriptor.product) != (USB_VID, USB_PID):
                    continue
                handle = ctypes.c_void_p()
                opened = library.libusb_open(devices[index], ctypes.byref(handle))
                if opened != 0:
                    result = {"matching_device_reset": False, "open_error": opened}
                    continue
                try:
                    buffer = ctypes.create_string_buffer(256)
                    length = library.libusb_get_string_descriptor_ascii(
                        handle, descriptor.serial_index, buffer, len(buffer))
                    if length < 0 or buffer.raw[:length].decode(errors="replace") != self.serial:
                        result = {"matching_device_reset": False,
                                  "serial_check_error": "unavailable-or-mismatch"}
                        continue
                    code = library.libusb_reset_device(handle)
                    result = {"matching_device_reset": code == 0, "reset_result": code}
                    break
                finally:
                    library.libusb_close(handle)
        finally:
            library.libusb_free_device_list(devices, 1)
            library.libusb_exit(context)
        return result


class Recorder:
    """Timestamped run directory with redacted command logs and a report."""

    def __init__(self, action: str, log_dir: "str | None" = None):
        self.action = action
        self.data = {"action": action, "started_utc": utc_now()}
        self.directory = None
        if log_dir:
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
            self.directory = Path(log_dir).expanduser() / f"{stamp}-{action}"
        elif action in ("flash", "restore", "preflight", "reboot"):
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
            self.directory = ROOT / "out/anne-device-private/runs" / f"{stamp}-{action}"
        if self.directory:
            self.directory.mkdir(parents=True, exist_ok=True)
        self.data["log_dir"] = str(self.directory) if self.directory else None

    def command_log(self, label: str, output: str) -> None:
        if self.directory:
            (self.directory / f"{label}.txt").write_text(output)

    def finish(self, ok: bool, **fields) -> dict:
        self.data.update(fields)
        self.data.update({"ok": ok, "finished_utc": utc_now()})
        if self.directory:
            (self.directory / "report.json").write_text(json.dumps(self.data, indent=2) + "\n")
        return self.data


class Device:
    def __init__(self, fastboot: str, serial: str, recorder: "Recorder | None" = None,
                 reset=None, run=subprocess.run, settle: float = 3.0):
        self.fastboot = fastboot
        self.serial = serial
        self.recorder = recorder
        self.reset = reset
        self.run = run
        self.settle = settle

    def devices(self) -> list:
        return list_devices(self.fastboot, run=self.run)

    def wait_until_present(self, timeout: "float | None" = None) -> bool:
        deadline = time.monotonic() + (self.settle if timeout is None else timeout)
        while time.monotonic() < deadline:
            try:
                if self.serial in self.devices():
                    return True
            except Exception:
                pass
            time.sleep(0.1)
        return False

    def command(self, args, label: str, timeout: int = 15, reset: bool = True) -> str:
        if reset and self.reset is not None:
            outcome = self.reset()
            if not outcome.get("matching_device_reset"):
                raise FastbootError(
                    "USB reset did not match the target serial: "
                    + json.dumps(outcome, sort_keys=True))
            if not self.wait_until_present():
                raise FastbootError("device did not re-enumerate after the USB reset")
        try:
            result = self.run([self.fastboot, "--unbuffered", "-s", self.serial, *args],
                              capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise FastbootError(f"{label}: timed out after {timeout}s; no further commands issued")
        output = redact(decode(result.stdout) + decode(result.stderr), self.serial)
        if self.recorder:
            self.recorder.command_log(label, output)
        # A remote FAILED can still exit 0, so inspect the text as well.
        if result.returncode != 0 or re.search(r"\bFAILED\b", output) or "OKAY" not in output:
            raise FastbootError(f"{label}: not acknowledged\n{output.strip()}")
        return output

    def preflight(self, expect_model: "str | None" = MODEL,
                  expect_firmware: "str | None" = None) -> dict:
        model = oem_value(self.command(["oem", "get-product-model"], "model"))
        firmware = oem_value(self.command(["oem", "get-build-number"], "firmware"))
        boot = self.command(["oem", "get-bootinfo"], "bootinfo")
        unlocked = unlock_state(boot)
        if expect_model and expect_model not in model:
            raise FastbootError(f"unexpected model {model!r}; expected {expect_model!r}")
        if expect_firmware and expect_firmware not in firmware:
            raise FastbootError(f"unexpected firmware {firmware!r}; expected {expect_firmware!r}")
        if not unlocked:
            raise FastbootError(f"bootloader is not unlocked: {boot.strip()!r}")
        return {"model": model.strip(), "firmware": firmware.strip(),
                "bootloader_unlocked": unlocked}


def oem_value(output: str) -> str:
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("(bootloader)"):
            line = line[len("(bootloader)"):].strip()
        if line.startswith(":"):
            line = line[1:].strip()
        if line and not line.startswith(("OKAY", "Finished", "FAILED", "< waiting")):
            return line
    return ""


def unlock_state(output: str) -> bool:
    states = set(re.findall(r"\b(unlocked|locked)\b", output.lower()))
    return states == {"unlocked"}


def validate_package(package: "str | Path", kind: str,
                     expected_bytes: int = KERNEL_PARTITION_BYTES,
                     expect_sha256: "str | None" = None) -> dict:
    if kind not in IMAGE_KINDS:
        raise FastbootError(f"unknown image kind {kind!r}")
    directory = Path(package).expanduser().resolve()
    manifest_path = directory / "manifest.json"
    if not manifest_path.is_file():
        raise FastbootError(f"no manifest.json in {directory}")
    try:
        manifest = json.loads(manifest_path.read_text())
    except ValueError as error:
        raise FastbootError(f"manifest.json is not valid JSON: {error}")
    image_name, hash_key = IMAGE_KINDS[kind]
    image = directory / image_name
    if not image.is_file():
        raise FastbootError(f"missing {image_name} in {directory}")
    if manifest.get("board") != BOARD:
        raise FastbootError(f"package board is {manifest.get('board')!r}, not {BOARD!r}")
    if manifest.get("model") != MODEL:
        raise FastbootError(f"package model is {manifest.get('model')!r}, not {MODEL!r}")
    if manifest.get("partition") != PARTITION:
        raise FastbootError(f"package targets partition {manifest.get('partition')!r}, not {PARTITION!r}")
    declared = manifest.get("partition_bytes")
    if declared != expected_bytes:
        raise FastbootError(
            f"package is built for {declared} bytes, this handset's {PARTITION} "
            f"partition is {expected_bytes}")
    if image.stat().st_size != declared:
        raise FastbootError(f"{image_name} is {image.stat().st_size} bytes, expected {declared}")
    digest = sha256_file(image)
    if digest != manifest.get(hash_key):
        raise FastbootError(f"{image_name} sha256 does not match manifest {hash_key}")
    if expect_sha256 and digest != expect_sha256.lower():
        raise FastbootError(f"{image_name} sha256 {digest} does not match --expect-sha256")
    if (manifest.get("avb") or {}).get("local_signature_and_hash_verified") is not True:
        raise FastbootError("package AVB signature/hash was not verified locally")
    # Optional provenance cross-checks when the package carries the raw inputs.
    for name, key in (("kernel.bin", "raw_kernel_sha256"), ("system.mxb", "signed_bundle_sha256")):
        artifact = directory / name
        if artifact.is_file() and manifest.get(key):
            if sha256_file(artifact) != manifest[key]:
                raise FastbootError(f"{name} sha256 does not match manifest {key}")
    return {"package": str(directory), "image": str(image),
            "image_sha256": digest, "image_bytes": image.stat().st_size,
            "manifest": manifest}


def connect(args) -> tuple:
    fastboot = find_fastboot(args.fastboot)
    serial = resolve_serial(args.serial, fastboot)
    recorder = Recorder(args.command, args.log_dir)
    resetter = None if args.no_reset else UsbReset(serial)
    device = Device(fastboot, serial, recorder=recorder, reset=resetter,
                    settle=args.settle)
    recorder.data.update({"fastboot": fastboot, "serial_fingerprint": fingerprint(serial)})
    return device, recorder


def parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--serial", help="target serial (default: the only attached device)")
    common.add_argument("--fastboot", help="fastboot binary (default: bundled platform-tools, then PATH)")
    common.add_argument("--log-dir", help="write command logs and report.json here")
    common.add_argument("--no-reset", action="store_true",
                        help="skip the per-command USB reset (debugging only; commands usually fail)")
    common.add_argument("--settle", type=float, default=3.0,
                        help="seconds to wait for re-enumeration after a reset")
    common.add_argument("--json", action="store_true", help="print the report as JSON")

    root = argparse.ArgumentParser(description=__doc__,
                                   formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = root.add_subparsers(dest="command", required=True)

    sub.add_parser("devices", parents=[common], help="list devices in fastboot mode")

    pre = sub.add_parser("preflight", parents=[common],
                         help="read model, firmware and bootloader lock state")
    pre.add_argument("--expect-model", default=MODEL)
    pre.add_argument("--expect-firmware", default=None)

    for name, kind in (("flash", "native"), ("restore", "restore")):
        command = sub.add_parser(name, parents=[common],
                                 help=f"write the {kind} image to {PARTITION}")
        command.add_argument("--package", required=True, help="package directory with manifest.json")
        command.add_argument("--write", action="store_true",
                             help="actually flash; without it the package is only validated")
        command.add_argument("--expect-sha256", default=None,
                             help="pin the image sha256 (default: manifest value)")

    reboot = sub.add_parser("reboot", parents=[common], help="reboot the phone from fastboot")
    reboot.add_argument("--to", choices=("system", "bootloader", "recovery"), default="system")
    return root


def emit(args, report: dict) -> None:
    if args.json:
        print(json.dumps(report, indent=2))
        return
    for key, value in report.items():
        if key in ("manifest",):
            continue
        print(f"{key}: {value}")


def main(argv=None) -> int:
    args = parser().parse_args(argv)
    report = {"action": args.command}
    try:
        if args.command == "devices":
            fastboot = find_fastboot(args.fastboot)
            serials = list_devices(fastboot)
            if args.json:
                print(json.dumps({"devices": serials}))
            else:
                for serial in serials:
                    print(f"{serial}\tfastboot")
                if not serials:
                    print("no fastboot device")
            return 0

        if args.command in ("flash", "restore"):
            kind = "native" if args.command == "flash" else "restore"
            checked = validate_package(args.package, kind,
                                       expect_sha256=args.expect_sha256)
            report.update({k: v for k, v in checked.items() if k != "manifest"})
            report["partition"] = PARTITION
            if not args.write:
                recorder = Recorder(args.command, args.log_dir)
                report.update(recorder.finish(
                    True, dry_run=True,
                    note="package validated; pass --write to flash"))
                emit(args, report)
                print("dry run: no image was written")
                return 0
            device, recorder = connect(args)
            expected = checked["manifest"].get("firmware")
            info = device.preflight(expect_model=MODEL,
                                    expect_firmware=expected if isinstance(expected, str) else None)
            report.update(info)
            device.command(["flash", PARTITION, checked["image"]], args.command, timeout=90)
            report.update(recorder.finish(True, flash_acknowledged=True))
            emit(args, report)
            print("flashed; next: scripts/anne/tools/fastboot.py reboot --to system")
            return 0

        device, recorder = connect(args)
        if args.command == "preflight":
            info = device.preflight(expect_model=args.expect_model,
                                    expect_firmware=args.expect_firmware)
            report.update(info)
            report.update(recorder.finish(True, **info))
            emit(args, report)
            return 0

        if args.command == "reboot":
            arguments = {"system": ["reboot"],
                         "bootloader": ["reboot", "bootloader"],
                         "recovery": ["reboot", "recovery"]}[args.to]
            device.command(arguments, "reboot", timeout=15)
            report.update(recorder.finish(True, target=args.to))
            emit(args, report)
            return 0
    except FastbootError as error:
        report.update({"ok": False, "error": redact(str(error), args.serial)})
        print(f"error: {report['error']}", file=sys.stderr)
        if args.json:
            print(json.dumps(report, indent=2))
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    return 2


if __name__ == "__main__":
    sys.exit(main())

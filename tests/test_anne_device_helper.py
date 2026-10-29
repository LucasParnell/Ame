import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "anne_fastboot_helper", ROOT / "scripts/anne/tools/fastboot.py")
fastboot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fastboot)

SERIAL = "SYNTHETIC-SERIAL-0001"


class FakeReset:
    def __init__(self, matches=True):
        self.matches = matches
        self.calls = 0

    def __call__(self):
        self.calls += 1
        return {"matching_device_reset": self.matches, "reset_result": 0}


class FakeRun:
    def __init__(self, responses, devices=None):
        self.responses = responses
        self.devices = [SERIAL] if devices is None else devices
        self.calls = []

    def __call__(self, command, **kwargs):
        self.calls.append(list(command))
        if command[1:] == ["devices"]:
            body = "".join(f"{serial}\tfastboot\n" for serial in self.devices)
            return subprocess.CompletedProcess(command, 0, body, "")
        key = tuple(command[4:])
        if key not in self.responses:
            raise AssertionError(f"unexpected command {key}")
        stdout, stderr, code = self.responses[key]
        return subprocess.CompletedProcess(command, code, stdout, stderr)


def responses(**overrides):
    base = {
        ("oem", "get-product-model"): ("(bootloader) ANE-LX1\nOKAY [  0.005s]\nFinished.\n", "", 0),
        ("oem", "get-build-number"): ("(bootloader) :ANE-LX1 8.0.0.151(C782)\nOKAY\n", "", 0),
        ("oem", "get-bootinfo"): ("(bootloader)  unlocked\nOKAY\n", "", 0),
    }
    base.update(overrides)
    return base


def make_package(directory: Path, size: int, avb: bool = True) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    image = b"AME!" + b"\0" * (size - 4)
    (directory / "ame-native-kernel.img").write_bytes(image)
    (directory / "restore-stock-kernel.img").write_bytes(b"R" * size)
    (directory / "kernel.bin").write_bytes(b"kernel-bytes")
    (directory / "system.mxb").write_bytes(b"bundle-bytes")
    manifest = {
        "schema": 1,
        "board": "huawei-anne",
        "model": "ANE-LX1",
        "partition": "kernel",
        "partition_bytes": size,
        "native_image_sha256": hashlib.sha256(image).hexdigest(),
        "restore_kernel_sha256": hashlib.sha256(b"R" * size).hexdigest(),
        "raw_kernel_sha256": hashlib.sha256(b"kernel-bytes").hexdigest(),
        "signed_bundle_sha256": hashlib.sha256(b"bundle-bytes").hexdigest(),
        "avb": {"local_signature_and_hash_verified": avb},
    }
    (directory / "manifest.json").write_text(json.dumps(manifest))
    return directory


class DeviceCommands(unittest.TestCase):
    def make(self, work, extra=None):
        answers = responses()
        answers.update(extra or {})
        runner = FakeRun(answers)
        reset = FakeReset()
        recorder = fastboot.Recorder("flash", str(Path(work) / "logs"))
        device = fastboot.Device("/fake/fastboot", SERIAL, recorder=recorder,
                                 reset=reset, run=runner, settle=0.05)
        return device, runner, reset, recorder

    def test_reset_precedes_every_command(self):
        with tempfile.TemporaryDirectory() as work:
            device, runner, reset, _ = self.make(work)
            device.command(["oem", "get-bootinfo"], "first")
            device.command(["oem", "get-product-model"], "second")
            self.assertEqual(reset.calls, 2)
            commands = [call for call in runner.calls if call[1:] != ["devices"]]
            self.assertEqual(len(commands), 2)
            for call in commands:
                self.assertEqual(call[1:4], ["--unbuffered", "-s", SERIAL])

    def test_failed_reset_stops_before_any_command(self):
        with tempfile.TemporaryDirectory() as work:
            runner = FakeRun(responses())
            device = fastboot.Device("/fake/fastboot", SERIAL, reset=FakeReset(False),
                                     run=runner, settle=0.01)
            with self.assertRaises(fastboot.FastbootError):
                device.command(["oem", "get-bootinfo"], "bootinfo")
            self.assertFalse([c for c in runner.calls if c[1:] != ["devices"]])

    def test_remote_failure_with_zero_exit_is_an_error(self):
        with tempfile.TemporaryDirectory() as work:
            device, _, _, _ = self.make(
                work, {("flash", "kernel", "/tmp/kernel.img"):
                       ("FAILED (remote: 'Command not allowed')\n", "", 0)})
            with self.assertRaises(fastboot.FastbootError):
                device.command(["flash", "kernel", "/tmp/kernel.img"], "flash", 90)

    def test_missing_okay_is_an_error(self):
        with tempfile.TemporaryDirectory() as work:
            device, _, _, _ = self.make(
                work, {("oem", "get-bootinfo"): ("(bootloader) unlocked\n", "", 0)})
            with self.assertRaises(fastboot.FastbootError):
                device.command(["oem", "get-bootinfo"], "bootinfo")

    def test_serial_is_redacted_in_logs(self):
        with tempfile.TemporaryDirectory() as work:
            device, _, _, recorder = self.make(
                work, {("oem", "get-bootinfo"): (f"(bootloader) {SERIAL} unlocked\nOKAY\n", "", 0)})
            device.command(["oem", "get-bootinfo"], "bootinfo")
            text = (recorder.directory / "bootinfo.txt").read_text()
            self.assertNotIn(SERIAL, text)
            self.assertIn("<device>", text)

    def test_preflight_reads_identity_and_lock_state(self):
        with tempfile.TemporaryDirectory() as work:
            device, _, _, _ = self.make(work)
            info = device.preflight()
            self.assertEqual(info["model"], "ANE-LX1")
            self.assertEqual(info["firmware"], "ANE-LX1 8.0.0.151(C782)")
            self.assertTrue(info["bootloader_unlocked"])

    def test_preflight_refuses_locked_bootloader(self):
        with tempfile.TemporaryDirectory() as work:
            device, _, _, _ = self.make(
                work, {("oem", "get-bootinfo"): ("(bootloader)  locked\nOKAY\n", "", 0)})
            with self.assertRaises(fastboot.FastbootError):
                device.preflight()


class LockParsing(unittest.TestCase):
    def test_only_a_single_unlocked_state_counts(self):
        self.assertTrue(fastboot.unlock_state("(bootloader)  unlocked\nOKAY"))
        self.assertFalse(fastboot.unlock_state("(bootloader) unlocked\n(bootloader) locked\nOKAY"))
        self.assertFalse(fastboot.unlock_state("(bootloader) relocked\nOKAY"))
        self.assertFalse(fastboot.unlock_state("OKAY"))


class PackageValidation(unittest.TestCase):
    def test_accepts_a_matching_package(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", 8)
            checked = fastboot.validate_package(package, "native", expected_bytes=8)
            self.assertEqual(checked["image_bytes"], 8)
            self.assertEqual(checked["image"], str(package.resolve() / "ame-native-kernel.img"))

    def test_rejects_wrong_size(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", 8)
            (package / "ame-native-kernel.img").write_bytes(b"too short")
            with self.assertRaises(fastboot.FastbootError):
                fastboot.validate_package(package, "native", expected_bytes=8)

    def test_rejects_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", 8)
            manifest = json.loads((package / "manifest.json").read_text())
            manifest["native_image_sha256"] = "0" * 64
            (package / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(fastboot.FastbootError):
                fastboot.validate_package(package, "native", expected_bytes=8)

    def test_rejects_unverified_avb(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", 8, avb=False)
            with self.assertRaises(fastboot.FastbootError):
                fastboot.validate_package(package, "native", expected_bytes=8)

    def test_rejects_wrong_partition(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", 8)
            manifest = json.loads((package / "manifest.json").read_text())
            manifest["partition"] = "boot"
            (package / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(fastboot.FastbootError):
                fastboot.validate_package(package, "native", expected_bytes=8)

    def test_rejects_wrong_partition_bytes_for_this_handset(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", 8)
            with self.assertRaises(fastboot.FastbootError):
                fastboot.validate_package(package, "native")


class CliGates(unittest.TestCase):
    class ExplodingDevice:
        def command(self, *args, **kwargs):
            raise AssertionError("dry run must not touch the phone")

    def test_flash_without_write_is_a_dry_run(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", fastboot.KERNEL_PARTITION_BYTES)
            original = fastboot.connect
            fastboot.connect = lambda args: (
                self.ExplodingDevice(),
                fastboot.Recorder("flash", str(Path(work) / "logs")))
            try:
                self.assertEqual(fastboot.main(["flash", "--package", str(package)]), 0)
            finally:
                fastboot.connect = original

    def test_flash_with_write_preflights_then_flashes(self):
        with tempfile.TemporaryDirectory() as work:
            package = make_package(Path(work) / "package", fastboot.KERNEL_PARTITION_BYTES)
            calls = []

            class Device:
                def preflight(self, **kwargs):
                    calls.append(("preflight", kwargs))
                    return {"model": "ANE-LX1", "firmware": "x", "bootloader_unlocked": True}

                def command(self, arguments, label, timeout):
                    calls.append(("command", arguments, label, timeout))

            original = fastboot.connect
            fastboot.connect = lambda args: (
                Device(), fastboot.Recorder("flash", str(Path(work) / "logs")))
            try:
                code = fastboot.main(["flash", "--package", str(package), "--write"])
            finally:
                fastboot.connect = original
            self.assertEqual(code, 0)
            self.assertEqual(calls[0][0], "preflight")
            self.assertEqual(calls[1][1], ["flash", "kernel",
                                            str(package.resolve() / "ame-native-kernel.img")])
            self.assertEqual(calls[1][2:], ("flash", 90))


if __name__ == "__main__":
    unittest.main()

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts/anne'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('anne_flash', SCRIPTS / 'flash-el2.py')
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)


class FakeFactory:
    def __init__(self, image, corrupt=False, wrong_size=False):
        self.image = image
        self.corrupt = corrupt
        self.wrong_size = wrong_size
        self.commands = []
        self.reads = []
        self.wr = self
        self.rd = self
        self.flashed = False

    def command(self, command):
        self.commands.append(command)
        if command == 'getvar:emmc:kernel':
            size = len(self.image) + int(self.wrong_size)
            return f'{0x100000:016x}:{size:016x}'.encode()
        if command == 'flash:kernel':
            self.flashed = True
        return b''

    def read(self, *args, **kwargs):
        if len(args) == 1:  # download response
            return f'DATA{len(self.image):08x}'.encode()
        kind, address, size = args
        self.reads.append((self.flashed, kind, address, size))
        if not self.flashed or self.corrupt:
            return bytes(size)
        offset = address - 0x100000
        return self.image[offset:offset + size]

    def write(self, data, **kwargs):
        return len(data)

    def status(self):
        return b''


class AnneFlash(unittest.TestCase):
    def test_bootrom_waits_for_tty_creation_and_udev_permissions(self):
        port = SimpleNamespace(vid=0x12d1, pid=0x3609, device='/dev/ttyUSB0')
        discover = mock.Mock(side_effect=[[], [port], [port]])
        with mock.patch.object(flash.os, 'access', side_effect=[False, True]), \
             mock.patch.object(flash.time, 'monotonic', side_effect=[0, 0, 1]), \
             mock.patch.object(flash.time, 'sleep') as sleep:
            self.assertEqual(flash.wait_bootrom_tty(discover), '/dev/ttyUSB0')
            self.assertEqual(sleep.call_count, 2)

    def test_bootrom_ambiguity_and_inaccessible_timeout_stop_before_upload(self):
        port = SimpleNamespace(vid=0x12d1, pid=0x3609, device='/dev/ttyUSB0')
        with self.assertRaisesRegex(RuntimeError, 'Multiple'):
            flash.wait_bootrom_tty(lambda: [port, port])
        with mock.patch.object(flash.os, 'access', return_value=False), \
             mock.patch.object(flash.time, 'monotonic', side_effect=[0, 16]):
            with self.assertRaisesRegex(RuntimeError, 'accessible'):
                flash.wait_bootrom_tty(lambda: [port])

    def test_default_skips_old_kernel_and_verifies_flash(self):
        image = b'new kernel' * 64
        fb = FakeFactory(image)
        report = {}
        with tempfile.TemporaryDirectory() as work:
            flash.install(fb, image, Path(work), report, lambda: None)
            self.assertFalse((Path(work) / 'kernel-before.img').exists())
            self.assertEqual((Path(work) / 'kernel-after.img').read_bytes(), image)
        self.assertTrue(all(after_flash for after_flash, *_ in fb.reads))
        self.assertTrue(report['readback_verified'])

    def test_backup_is_opt_in(self):
        image = b'new kernel' * 64
        fb = FakeFactory(image)
        with tempfile.TemporaryDirectory() as work:
            flash.install(fb, image, Path(work), {}, lambda: None, backup=True)
            self.assertEqual((Path(work) / 'kernel-before.img').stat().st_size, len(image))
        self.assertFalse(fb.reads[0][0])
        self.assertTrue(fb.reads[-1][0])

    def test_readback_mismatch_fails(self):
        image = b'new kernel' * 64
        report = {}
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(RuntimeError, 'readback mismatch'):
                flash.install(FakeFactory(image, corrupt=True), image, Path(work), report, lambda: None)
        self.assertFalse(report['readback_verified'])

    def test_wrong_partition_size_stops_before_flash(self):
        image = b'new kernel' * 64
        fb = FakeFactory(image, wrong_size=True)
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(RuntimeError, 'partition size'):
                flash.install(fb, image, Path(work), {}, lambda: None)
        self.assertFalse(fb.flashed)
        self.assertFalse(fb.reads)
